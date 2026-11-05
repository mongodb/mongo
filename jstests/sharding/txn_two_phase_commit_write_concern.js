/**
 * Tests that coordinateCommitTransaction returns the decision once the decision has been written
 * with the client's writeConcern.
 *
 * In disaggregated storage the majority commit point is advanced by the log service rather than by
 * secondary acknowledgements, and w > 1 is remapped to w: "majority". So instead of stopping
 * replication on a subset of secondaries, the DSC variant freezes the coordinator primary's commit
 * point and checks whether commitTransaction returns before the decision is majority committed.
 * @tags: [uses_transactions, uses_multi_shard_transaction, resource_intensive]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {isDisagg} from "jstests/libs/oplog_truncation_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {enableCoordinateCommitReturnImmediatelyAfterPersistingDecision} from "jstests/sharding/libs/sharded_transactions_helpers.js";

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
        settings: {chainingAllowed: false},
    },
    causallyConsistent: true,
});
enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st);

const kDbName = jsTest.name();
const kCollName = "test";
const kNs = kDbName + "." + kCollName;

const lsid = {
    id: UUID(),
};
let txnNumber = 0;

assert.commandWorked(
    st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
);
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    st.s.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        writeConcern: {w: "majority"},
    }),
);

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
    const runCommitExpectSuccessCode =
        "assert.commandWorked(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " +
        tojson(lsid) +
        "," +
        "txnNumber: NumberLong(" +
        txnNumber +
        ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "writeConcern: " +
        tojson(writeConcern) +
        "}));";
    return startParallelShell(runCommitExpectSuccessCode, st.s.port);
}

/*
 * Runs a transaction to inserts the given docs.
 */
function runInsertCmdInTxn(docs) {
    assert.commandWorked(
        st.s.getDB(kDbName).runCommand({
            insert: kCollName,
            documents: docs,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }),
    );
}

/*
 * Returns the 'decision' inside the coordinator doc with the given 'lsid' and 'txnNumber'
 * on this connection. Returns null if the coordinator doc does not exist or does not have
 * the 'decision' field.
 */
function getDecision(nodeConn, lsid, txnNumber) {
    const coordDoc = nodeConn
        .getCollection("config.transaction_coordinators")
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
function getNumNodesWithCommitDecision() {
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
function assertDecisionMajorityCommitted(coordinatorRs) {
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
        `Verify that commitTransaction returns once the decision is written with client's writeConcern ${tojson(
            writeConcern,
        )}`,
    );
    awaitResult();
    if (writeConcern.w == "majority") {
        // When using majority write concern, secondaries acknowledge/make durable writes
        // independently from applying them, so before checking for the commit decision we need to
        // ensure lastApplied is caught up on the relevant node(s).
        const nodesToAwait = st.rs0.getSecondaries().slice((-1 * st.rs0.nodes.length) / 2);
        st.rs0.awaitReplication(null, null, nodesToAwait);
    }
    assertDecisionCommittedOnNodes(st.rs0, st.rs0.nodes.length - nodesToStopReplication.length);

    jsTest.log(
        "Verify that the coordinator doc is majority committed regardless of the client's writeConcern",
    );
    // Re-enable replication to allow the decision to be majority committed and two-phase
    // commit to finish.
    if (nodesToStopReplication.length > 0) {
        restartServerReplication(nodesToStopReplication);
    }
    st.rs0.awaitReplication();
    deleteCoordDocFailPoint.wait();
    assertDecisionMajorityCommitted(st.rs0);
    deleteCoordDocFailPoint.off();

    jsTest.log("Verify the insert operation was committed successfully");
    let res = assert.commandWorked(
        st.s.getDB(kDbName).runCommand({find: kCollName, filter: {$or: docs}, lsid: lsid}),
    );
    assert.eq(2, res.cursor.firstBatch.length);

    txnNumber++;
}

/*
 * Returns the 'decision' inside the coordinator doc read at majority read concern on the
 * coordinator primary. Uses a fresh, non-causally-consistent connection so the read does not wait
 * for the commit point to reach the session's operationTime.
 */
function getMajorityCommittedDecision(rs) {
    const conn = new Mongo(rs.getPrimary().host);
    const res = assert.commandWorked(
        conn.getDB("config").runCommand({
            find: "transaction_coordinators",
            filter: {"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber},
            readConcern: {level: "majority"},
        }),
    );
    const coordDoc = res.cursor.firstBatch[0];
    return coordDoc ? coordDoc.decision : null;
}

/*
 * Returns true if the commitTransaction for the current transaction is still running on mongos.
 */
function isCommitInProgressOnMongos() {
    const ops = st.s
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, localOps: true}},
            {$match: {"command.commitTransaction": {$exists: true}, "lsid.id": lsid.id}},
        ])
        .toArray();
    return ops.length > 0;
}

function testCommitDecisionWriteConcernDisagg(writeConcern) {
    jsTest.log.info("Testing commitTransaction (disagg)", {writeConcern});
    // DSC remaps any w > 1 to w: "majority", so only w: 1 can return before the decision is
    // majority committed.
    const expectWaitForMajority = writeConcern.w !== 1;

    const x = txnNumber + 1;
    const docs = [{x: -x}, {x: x}];
    runInsertCmdInTxn(docs);

    // Pause right before the coordinator persists the decision so we can freeze the commit point
    // before the decision write happens.
    const persistDecisionFailPoint = configureFailPoint(st.shard0, "hangBeforeWritingDecision");
    // The coordinator always waits for the decision to be majority committed before sending commit
    // to participants and deleting its coordinator doc.
    const deleteCoordDocFailPoint = configureFailPoint(
        st.shard0,
        "hangBeforeDeletingCoordinatorDoc",
    );

    const awaitResult = runCommitThroughMongosInParallelShellExpectSuccess(writeConcern);
    persistDecisionFailPoint.wait();

    // Freeze the coordinator primary's commit point. The decision write happens after this, so it
    // can be applied locally but cannot become majority committed.
    const commitPointFailPoint = configureFailPoint(
        st.rs0.getPrimary(),
        "hangBeforeUpdatingCommittedOpTimeFromLogServer",
    );
    persistDecisionFailPoint.off();
    commitPointFailPoint.wait();

    // The decision is written locally on the coordinator primary.
    assert.soon(() => {
        const decision = getDecision(st.rs0.getPrimary(), lsid, txnNumber);
        return decision && isCommitDecision(decision);
    });

    if (expectWaitForMajority) {
        jsTest.log(
            "Verify that commitTransaction does not return while the decision is not majority committed",
        );
        // Keep checking for a short window to give an incorrect early return a chance to surface.
        const deadline = Date.now() + 2000;
        while (Date.now() < deadline) {
            assert(
                isCommitInProgressOnMongos(),
                "commitTransaction returned before majority commit",
            );
            sleep(100);
        }
    } else {
        jsTest.log(
            "Verify that commitTransaction returns before the decision is majority committed",
        );
        awaitResult();
    }
    assert.eq(null, getMajorityCommittedDecision(st.rs0));

    jsTest.log(
        "Verify that the coordinator doc is majority committed regardless of the client's writeConcern",
    );
    commitPointFailPoint.off();
    if (expectWaitForMajority) {
        awaitResult();
    }
    deleteCoordDocFailPoint.wait();
    const majorityDecision = getMajorityCommittedDecision(st.rs0);
    assert(
        majorityDecision && isCommitDecision(majorityDecision),
        "expected a majority committed commit decision",
        {
            majorityDecision,
        },
    );
    deleteCoordDocFailPoint.off();

    jsTest.log("Verify the insert operation was committed successfully");
    const res = assert.commandWorked(
        st.s.getDB(kDbName).runCommand({find: kCollName, filter: {$or: docs}, lsid: lsid}),
    );
    assert.eq(2, res.cursor.firstBatch.length);

    txnNumber++;
}

const testFn = isDisagg(st.rs0.getPrimary())
    ? testCommitDecisionWriteConcernDisagg
    : testCommitDecisionWriteConcern;
testFn({w: 1});
testFn({w: "majority"});
// In DSC, w: 3 is remapped to w: "majority".
testFn({w: 3});

st.stop();
