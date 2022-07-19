/**
 * Shuts down the donor primary at two points in the critical section: while the node is executing
 * _configsvrEnsureChunkVersionIsGreaterThan and while the node is forcing a filtering metadata
 * refresh.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */

(function() {
'use strict';

// This test shuts down a shard primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

load('jstests/libs/parallel_shell_helpers.js');
load('jstests/libs/fail_point_util.js');

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

const dbName = "test";

function testShutDownAfterFailPoint(failPointName) {
    let st = new ShardingTest({shards: 2});

    const donorShard = st.shard0;
    const recipientShard = st.shard1;

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: donorShard.shardName}));

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
    configureFailPoint(st.rs0.getPrimary(), "migrationCommitNetworkError");

    // Set the requested failpoint and launch the moveChunk asynchronously.
    let failPoint = configureFailPoint(st.rs0.getPrimary(), failPointName);
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandWorked(db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
        }, ns, recipientShard.shardName), st.s.port);

    jsTest.log("Waiting for moveChunk to reach " + failPointName + " failpoint");
    failPoint.wait();

    // Ensure we are able to shut down the donor primary by asserting that its exit code is 0.
    assert.eq(0, MongoRunner.stopMongod(st.rs0.getPrimary(), null, {}, true /* waitpid */));
    awaitResult();

    st.stop();
}

testShutDownAfterFailPoint("hangInEnsureChunkVersionIsGreaterThanInterruptible");
testShutDownAfterFailPoint("hangInRefreshFilteringMetadataUntilSuccessInterruptible");
testShutDownAfterFailPoint("hangInPersistMigrateCommitDecisionInterruptible");
testShutDownAfterFailPoint("hangInDeleteRangeDeletionOnRecipientInterruptible");
testShutDownAfterFailPoint("hangInReadyRangeDeletionLocallyInterruptible");
testShutDownAfterFailPoint("hangInAdvanceTxnNumInterruptible");
})();
