/**
 * Sends killOp on the coordinator's OperationContext's opId at each point in the commit
 * coordination where there is an OperationContext and ensures the coordination still runs to
 * completion for all the points.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    getCoordinatorFailpoints,
    waitForFailpoint
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {
    runCommitThroughMongosInParallelThread
} from "jstests/sharding/libs/txn_two_phase_commit_util.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 3, causallyConsistent: true});

let coordinator = st.shard0;
let participant1 = st.shard1;
let participant2 = st.shard2;

let lsid = {id: UUID()};
let txnNumber = 0;

const setUp = function() {
    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: coordinator.shardName}));
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
    st.refreshCatalogCacheForNs(st.s, ns);

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
        mode: "alwaysOn",
        data: failpointData.data ? failpointData.data : {}
    }));

    // Run commitTransaction through a parallel shell.
    let commitThread;
    if (shouldCommit) {
        commitThread = runCommitThroughMongosInParallelThread(lsid, txnNumber, st.s.host);
    } else {
        commitThread = runCommitThroughMongosInParallelThread(
            lsid, txnNumber, st.s.host, ErrorCodes.NoSuchTransaction);
    }
    commitThread.start();

    // Deliver killOp once the failpoint has been hit.

    waitForFailpoint("Hit " + failpointData.failpoint + " failpoint",
                     failpointData.numTimesShouldBeHit);

    jsTest.log("Going to find coordinator opCtx ids");
    let coordinatorOpsToKill = [];
    assert.soon(() => {
        coordinatorOpsToKill = coordinator.getDB("admin")
                                   .aggregate([
                                       {$currentOp: {'allUsers': true, 'idleConnections': true}},
                                       {
                                           $match: {
                                               $and: [
                                                   {desc: "TransactionCoordinator"},
                                                   // Filter out the prepareTransaction op on the
                                                   // coordinator itself since killing it would
                                                   // cause the transaction to abort.
                                                   {"command.prepareTransaction": {$exists: false}}
                                               ]
                                           }
                                       }
                                   ])
                                   .toArray();

        for (let x = 0; x < coordinatorOpsToKill.length; x++) {
            if (!coordinatorOpsToKill[x].opid) {
                print("Retrying currentOp because op doesn't have opId: " +
                      tojson(coordinatorOpsToKill[x]));
                return false;
            }
        }

        return true;
    }, 'timed out trying to fetch coordinator ops', undefined /* timeout */, 1000 /* interval */);

    // Use "greater than or equal to" since, for failpoints that pause the coordinator while
    // it's sending prepare or sending the decision, there might be one additional thread that's
    // doing the "send" to the local participant (or that thread might have already completed).
    assert.gte(coordinatorOpsToKill.length, failpointData.numTimesShouldBeHit);

    coordinatorOpsToKill.forEach(function(coordinatorOp) {
        coordinator.getDB("admin").killOp(coordinatorOp.opid);
    });
    assert.commandWorked(coordinator.adminCommand({
        configureFailPoint: failpointData.failpoint,
        mode: "off",
    }));

    // If the commit coordination was not robust to killOp, then commitTransaction would fail
    // with an Interrupted error rather than fail with NoSuchTransaction or return success.
    jsTest.log("Wait for the commit coordination to complete.");
    commitThread.join();

    // If deleting the coordinator doc was not robust to killOp, the document would still exist.
    // Deletion is done asynchronously, so we might have to wait.
    assert.soon(
        () => coordinator.getDB("config").getCollection("transaction_coordinators").count() == 0);

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

// Test commit path.
failpointDataArr.forEach(function(failpointData) {
    testCommitProtocol(true /* shouldCommit */, failpointData);
    clearRawMongoProgramOutput();
});

st.stop();
