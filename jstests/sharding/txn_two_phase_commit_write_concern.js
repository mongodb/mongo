/*
 * Tests that coordinateCommitTransaction returns the decision once the decision has been written
 * with the client's writeConcern.
 * @tags: [uses_transactions, uses_multi_shard_transaction, requires_fcv_46]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/write_concern_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {
        // Set priority of secondaries to 0 so that the primary does not change during each
        // testcase.
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        // Disallow chaining to force both secondaries to sync from the primary. The testcase for
        // writeConcern "majority" disables replication on one of the secondaries, with chaining
        // that would effectively disable replication on both secondaries, causing the testcase to
        // to fail since writeConcern is unsatsifiable.
        settings: {chainingAllowed: false}
    },
    causallyConsistent: true
});
enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st);

const kDbName = jsTest.name();
const kCollName = "test";
const kNs = kDbName + "." + kCollName;

const lsid = {
    id: UUID()
};
let txnNumber = 0;

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

// Make both shards have chunks for the collection so that two-phase commit is required.
assert.commandWorked(st.s.adminCommand({split: kNs, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: kNs, find: {x: 0}, to: st.shard1.shardName}));

// Do an insert to force a refresh so the transaction doesn't fail due to StaleConfig.
assert.commandWorked(st.s.getCollection(kNs).insert({x: 0}));

/*
 * Runs commitTransaction on the mongos in a parallel shell, and asserts that it works.
 */
function runCommitThroughMongosInParallelShellExpectSuccess(writeConcern) {
    const runCommitExpectSuccessCode = "assert.commandWorked(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "writeConcern: " + tojson(writeConcern) + "}));";
    return startParallelShell(runCommitExpectSuccessCode, st.s.port);
}

/*
 * Runs a transaction to inserts the given docs.
 */
function runInsertCmdInTxn(docs) {
    assert.commandWorked(st.s.getDB(kDbName).runCommand({
        insert: kCollName,
        documents: docs,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));
}

/*
 * Returns the 'decision' inside the coordinator doc with the given 'lsid' and 'txnNumber'
 * on this connection. Returns null if the coordinator doc does not exist or does not have
 * the 'decision' field.
 */
function getDecision(nodeConn, lsid, txnNumber) {
    const coordDoc = nodeConn.getCollection("config.transaction_coordinators")
                         .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
    return coordDoc ? coordDoc.decision : null;
}

/*
 * Returns true if the given 'decision' represents a commit decision.
 */
function isCommitDecision(decision) {
    return decision.decision === "commit" && decision.commitTimestamp !== null;
}

/*
 * Returns the number of coordinator replica set nodes that have written the commit decision
 * to the config.transactions collection.
 */
function getNumNodesWithCommitDecision(coordinatorRs) {
    const decision = getDecision(st.rs0.getPrimary(), lsid, txnNumber);
    assert(isCommitDecision(decision));
    let numNodes = 1;

    for (const node of st.rs0.getSecondaries()) {
        const secDecision = getDecision(node, lsid, txnNumber);
        if (secDecision) {
            assert.eq(0, bsonWoCompare(secDecision, decision));
            numNodes++;
        }
    }

    return numNodes;
}

/*
 * Asserts that the coordinator doc has been replicated to the given number of nodes.
 */
function assertDecisionCommittedOnNodes(coordinatorRs, numNodes) {
    assert.eq(getNumNodesWithCommitDecision(coordinatorRs), numNodes);
}

/*
 * Asserts that the coordinator doc has been majority replicated.
 */
function assertDecisionMajorityCommitted(coordinatorRs, numNodes) {
    assert.gte(getNumNodesWithCommitDecision(coordinatorRs), coordinatorRs.nodes.length / 2);
}

/*
 * Returns an array of nodes that we can stop replication on and still allow writes on
 * the replica set to satsify the given write concern.
 */
function getNodesToStopReplication(rs, writeConcern) {
    if (writeConcern.w == "majority") {
        return rs.getSecondaries().slice(0, rs.nodes.length / 2);
    }
    return rs.getSecondaries().slice(0, rs.nodes.length - writeConcern.w);
}

function testCommitDecisionWriteConcern(writeConcern) {
    jsTest.log(`Testing commitTransaction with writeConcern ${tojson(writeConcern)}`);
    // Start a transaction that inserts documents.
    const x = txnNumber + 1;
    const docs = [{x: -x}, {x: x}];
    runInsertCmdInTxn(docs);

    // Turn on the failpoint to pause coordinateCommit right before the coordinator persists
    // the decision so we can disable replication on the nodes that are not needed for satifying
    // the write concern.
    let persistDecisionFailPoint = configureFailPoint(st.shard0, "hangBeforeWritingDecision");
    const nodesToStopReplication = getNodesToStopReplication(st.rs0, writeConcern);

    // Turn on the failpoint to pause coordinateCommit right before the coordinator deletes
    // its coordinator doc so we can check the doc has been majority committed before it gets
    // deleted.
    let deleteCoordDocFailPoint = configureFailPoint(st.shard0, "hangBeforeDeletingCoordinatorDoc");

    // Run commitTransaction with the given writeConcern. Disable replication on necessary nodes
    // right before it persists the decision.
    let awaitResult = runCommitThroughMongosInParallelShellExpectSuccess(writeConcern);
    persistDecisionFailPoint.wait();
    if (nodesToStopReplication.length > 0) {
        stopServerReplication(nodesToStopReplication);
    }
    persistDecisionFailPoint.off();

    jsTest.log(
        `Verify that commitTransaction returns once the decision is written with client's writeConcern ${
            tojson(writeConcern)}`);
    awaitResult();
    assertDecisionCommittedOnNodes(st.rs0, st.rs0.nodes.length - nodesToStopReplication.length);

    jsTest.log(
        "Verify that the coordinator doc is majority committed regardless of the client's writeConcern");
    // Re-enable replication to allow the decision to be majority committed and two-phase
    // commit to finish.
    if (nodesToStopReplication.length > 0) {
        restartServerReplication(nodesToStopReplication);
    }
    deleteCoordDocFailPoint.wait();
    assertDecisionMajorityCommitted(st.rs0);
    deleteCoordDocFailPoint.off();

    jsTest.log("Verify the insert operation was committed successfully");
    let res = assert.commandWorked(
        st.s.getDB(kDbName).runCommand({find: kCollName, filter: {$or: docs}, lsid: lsid}));
    assert.eq(2, res.cursor.firstBatch.length);

    txnNumber++;
}

testCommitDecisionWriteConcern({w: 1});
testCommitDecisionWriteConcern({w: "majority"});
testCommitDecisionWriteConcern({w: 3});

st.stop();
})();
