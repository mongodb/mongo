// Ensures that all pending move chunk operations get interrupted when the primary of the config
// server steps down and then becomes primary again. Then the migration can be rejoined, and a
// success/failure response still returned to the caller.
//
// Also tests the failure of a migration commit command on the source shard of a migration, due to
// the balancer being interrupted, failing to recover the active migrations, and releasing the
// distributed lock.

load('./jstests/libs/chunk_manipulation_util.js');
load("jstests/sharding/libs/find_chunks_util.js");

(function() {
'use strict';

// Intentionally use a config server with 1 node so that the step down and promotion to primary
// are guaranteed to happen on the same host
var st = new ShardingTest({shards: 2, other: {chunkSize: 1}});
var mongos = st.s0;

assert.commandWorked(mongos.adminCommand({enableSharding: 'TestDB'}));
st.ensurePrimaryShard('TestDB', st.shard0.shardName);
assert.commandWorked(mongos.adminCommand({shardCollection: 'TestDB.TestColl', key: {Key: 1}}));

var coll = mongos.getDB('TestDB').TestColl;

// For startParallelOps to write its state
var staticMongod = MongoRunner.runMongod({});

function interruptMoveChunkAndRecover(fromShard, toShard, isJumbo) {
    pauseMigrateAtStep(toShard, migrateStepNames.rangeDeletionTaskScheduled);

    var joinMoveChunk = moveChunkParallel(staticMongod,
                                          mongos.host,
                                          {Key: 0},
                                          null,
                                          'TestDB.TestColl',
                                          toShard.shardName,
                                          true /* expectSuccess */,
                                          isJumbo);
    waitForMigrateStep(toShard, migrateStepNames.rangeDeletionTaskScheduled);

    // Stepdown the primary in order to force the balancer to stop. Use a timeout of 5 seconds for
    // both step down operations, because mongos will retry to find the CSRS primary for up to 20
    // seconds and we have two successive ones.
    assert.commandWorked(st.configRS.getPrimary().adminCommand({replSetStepDown: 5, force: true}));

    // Ensure a new primary is found promptly
    st.configRS.getPrimary(30000);

    assert.eq(
        1,
        findChunksUtil
            .findChunksByNs(mongos.getDB('config'), 'TestDB.TestColl', {shard: fromShard.shardName})
            .itcount());
    assert.eq(
        0,
        findChunksUtil
            .findChunksByNs(mongos.getDB('config'), 'TestDB.TestColl', {shard: toShard.shardName})
            .itcount());

    // At this point, the balancer is in recovery mode. Ensure that stepdown can be done again and
    // the recovery mode interrupted.
    assert.commandWorked(st.configRS.getPrimary().adminCommand({replSetStepDown: 5, force: true}));

    // Ensure a new primary is found promptly
    st.configRS.getPrimary(30000);

    unpauseMigrateAtStep(toShard, migrateStepNames.rangeDeletionTaskScheduled);

    // Ensure that migration succeeded
    joinMoveChunk();

    assert.eq(
        0,
        findChunksUtil
            .findChunksByNs(mongos.getDB('config'), 'TestDB.TestColl', {shard: fromShard.shardName})
            .itcount());
    assert.eq(
        1,
        findChunksUtil
            .findChunksByNs(mongos.getDB('config'), 'TestDB.TestColl', {shard: toShard.shardName})
            .itcount());
}

// We have one non-jumbo chunk initially
assert.commandWorked(coll.insert({Key: 0, Value: 'Test value'}));
interruptMoveChunkAndRecover(st.shard0, st.shard1, false);

// Add a bunch of docs to this chunks so that it becomes jumbo
const largeString = 'X'.repeat(10000);
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 2000; i++) {
    bulk.insert({Key: 0, Value: largeString});
}
assert.commandWorked(bulk.execute());
interruptMoveChunkAndRecover(st.shard1, st.shard0, true);

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
