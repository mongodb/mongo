/**
 * Kills the OperationContext used by the donor shard to send
 * _configsvrEnsureChunkVersionIsGreaterThan and to force a filtering metadata refresh.
 *
 * Depends on the checkOrphansAreDeleted hook at the end of ShardingTest to verify that the orphans,
 * range deletion tasks, and migration coordinator state are deleted despite the killOps.
 */

(function() {
'use strict';

load('jstests/libs/parallel_shell_helpers.js');

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

const dbName = "test";

let st = new ShardingTest({shards: 2});

const donorShard = st.shard0;
const recipientShard = st.shard1;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: donorShard.shardName}));

function testKillOpAfterFailPoint(failPointName, opToKillThreadName) {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Testing with " + tojson(arguments) + " using ns " + ns);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Insert some docs into the collection.
    const numDocs = 1000;
    var bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Simulate a network error on sending commit to the config server, so that the donor tries to
    // recover the commit decision.
    configureFailPoint(donorShard, "migrationCommitNetworkError");

    // Set the requested failpoint and launch the moveChunk asynchronously.
    let failPoint = configureFailPoint(donorShard, failPointName);
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandWorked(db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
        }, ns, recipientShard.shardName), st.s.port);

    jsTest.log("Waiting for moveChunk to reach " + failPointName + " failpoint");
    failPoint.wait();

    // Kill the OperationContext being used for the commit decision recovery several times. Note, by
    // expecting to find a matching OperationContext multiple times, we are verifying that the
    // commit decision recovery is resumed with a fresh OperationContext after the previous
    // OperationContext was interrupted by the killOp.
    jsTest.log("Killing OperationContext for " + opToKillThreadName + " several times");
    for (let i = 0; i < 10; i++) {
        let matchingOps;
        assert.soon(() => {
            matchingOps = donorShard.getDB("admin")
                              .aggregate([
                                  {$currentOp: {'allUsers': true, 'idleConnections': true}},
                                  {$match: {desc: {$regex: opToKillThreadName}}}
                              ])
                              .toArray();
            // Wait for the opid to be present, since it's possible for currentOp to run after the
            // Client has been created but before it has been associated with a new
            // OperationContext.
            return 1 === matchingOps.length && matchingOps[0].opid != null;
        }, "Failed to find op with desc " + opToKillThreadName);
        donorShard.getDB("admin").killOp(matchingOps[0].opid);
    }

    failPoint.off();

    awaitResult();
}

testKillOpAfterFailPoint("hangInEnsureChunkVersionIsGreaterThanInterruptible",
                         "ensureChunkVersionIsGreaterThan");
testKillOpAfterFailPoint("hangInRefreshFilteringMetadataUntilSuccessInterruptible",
                         "refreshFilteringMetadataUntilSuccess");
testKillOpAfterFailPoint("hangInPersistMigrateCommitDecisionInterruptible",
                         "persist migrate commit decision");
testKillOpAfterFailPoint("hangInDeleteRangeDeletionOnRecipientInterruptible",
                         "cancel range deletion on recipient");
testKillOpAfterFailPoint("hangInReadyRangeDeletionLocallyInterruptible",
                         "ready local range deletion");
testKillOpAfterFailPoint("hangInAdvanceTxnNumInterruptible", "advance migration txn number");

st.stop();
})();
