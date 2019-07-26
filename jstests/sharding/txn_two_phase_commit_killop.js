/**
 * Sends killOp on the coordinator's OperationContext's opId at each point in the commit
 * coordination where there is an OperationContext and ensures the coordination still runs to
 * completion for all the points.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 3, causallyConsistent: true});

let coordinator = st.shard0;
let participant1 = st.shard1;
let participant2 = st.shard2;

let lsid = {id: UUID()};
let txnNumber = 0;

const runCommitThroughMongosInParallelShellExpectSuccess = function() {
    const runCommitExpectSuccessCode = "assert.commandWorked(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "}));";
    return startParallelShell(runCommitExpectSuccessCode, st.s.port);
};

const runCommitThroughMongosInParallelShellExpectAbort = function() {
    const runCommitExpectSuccessCode = "assert.commandFailedWithCode(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "})," +
        "ErrorCodes.NoSuchTransaction);";
    return startParallelShell(runCommitExpectSuccessCode, st.s.port);
};

const setUp = function() {
    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: coordinator.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    assert.commandWorked(coordinator.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant2.adminCommand({_flushRoutingTableCacheUpdates: ns}));

    // Start a new transaction by inserting a document onto each shard.
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));
};

const testCommitProtocol = function(shouldCommit, failpointData) {
    jsTest.log("Testing two-phase " + (shouldCommit ? "commit" : "abort") +
               " protocol with failpointData: " + tojson(failpointData));

    txnNumber++;
    setUp();

    if (!shouldCommit) {
        // Manually abort the transaction on one of the participants, so that the participant
        // fails to prepare.
        assert.commandWorked(participant2.adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }));
    }

    // Turn on failpoint to make the coordinator hang at a the specified point.
    assert.commandWorked(coordinator.adminCommand({
        configureFailPoint: failpointData.failpoint,
        mode: {skip: (failpointData.skip ? failpointData.skip : 0)},
    }));

    // Run commitTransaction through a parallel shell.
    let awaitResult;
    if (shouldCommit) {
        awaitResult = runCommitThroughMongosInParallelShellExpectSuccess();
    } else {
        awaitResult = runCommitThroughMongosInParallelShellExpectAbort();
    }

    // Deliver killOp once the failpoint has been hit.

    waitForFailpoint("Hit " + failpointData.failpoint + " failpoint",
                     failpointData.numTimesShouldBeHit);

    jsTest.log("Going to find coordinator opCtx ids");
    let coordinatorOps =
        coordinator.getDB("admin")
            .aggregate(
                [{$currentOp: {'allUsers': true}}, {$match: {desc: "TransactionCoordinator"}}])
            .toArray();

    // Use "greater than or equal to" since, for failpoints that pause the coordinator while
    // it's sending prepare or sending the decision, there might be one additional thread that's
    // doing the "send" to the local participant (or that thread might have already completed).
    assert.gte(coordinatorOps.length, failpointData.numTimesShouldBeHit);

    coordinatorOps.forEach(function(coordinatorOp) {
        coordinator.getDB("admin").killOp(coordinatorOp.opid);
    });
    assert.commandWorked(coordinator.adminCommand({
        configureFailPoint: failpointData.failpoint,
        mode: "off",
    }));

    // If the commit coordination was not robust to killOp, then commitTransaction would fail
    // with an Interrupted error rather than fail with NoSuchTransaction or return success.
    jsTest.log("Wait for the commit coordination to complete.");
    awaitResult();

    // If deleting the coordinator doc was not robust to killOp, the document would still exist.
    assert.eq(0, coordinator.getDB("config").getCollection("transaction_coordinators").count());

    // Check that the transaction committed or aborted as expected.
    if (!shouldCommit) {
        jsTest.log("Verify that the transaction was aborted on all shards.");
        assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
    } else {
        jsTest.log("Verify that the transaction was committed on all shards.");
        // Use assert.soon(), because although coordinateCommitTransaction currently blocks
        // until the commit process is fully complete, it will eventually be changed to only
        // block until the decision is *written*, at which point the test can pass the
        // operationTime returned by coordinateCommitTransaction as 'afterClusterTime' in the
        // read to ensure the read sees the transaction's writes (TODO SERVER-37165).
        assert.soon(function() {
            return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
        });
    }

    st.s.getDB(dbName).getCollection(collName).drop();
};

const failpointDataArr = getCoordinatorFailpoints();

// TODO(SERVER-39754): The abort path is unreliable, because depending on the stage at which the
// transaction is aborted, the failpoints might be hit more than the specified number of times.
//
// // Test abort path.

// failpointDataArr.forEach(function(failpointData) {
//     testCommitProtocol(false /* shouldCommit */, failpointData);
//     clearRawMongoProgramOutput();
// });

// Test commit path.

failpointDataArr.forEach(function(failpointData) {
    testCommitProtocol(true /* shouldCommit */, failpointData);
    clearRawMongoProgramOutput();
});

st.stop();
})();
