// Ensures that all pending move chunk operations get interrupted when the primary of the config
// server steps down and then becomes primary again

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    'use strict';

    // Intentionally use a config server with 1 node so that the step down and promotion to primary
    // are guaranteed to happen on the same host
    var st = new ShardingTest({config: 1, shards: 2});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard0.shardName);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {Key: 1}}));

    var coll = st.s0.getDB('TestDB').TestColl;

    // We have one chunk initially
    assert.writeOK(coll.insert({Key: 0, Value: 'Test value'}));

    pauseMigrateAtStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

    // For startParallelOps to write its state
    var staticMongod = MongoRunner.runMongod({});

    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {Key: 0}, null, 'TestDB.TestColl', st.shard1.shardName);
    waitForMigrateStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

    // Stepdown the primary in order to force the balancer to stop
    assert.throws(function() {
        assert.commandWorked(
            st.configRS.getPrimary().adminCommand({replSetStepDown: 10, force: true}));
    });

    // Ensure a new primary is found promptly
    st.configRS.getPrimary(30000);

    assert.eq(1, st.s0.getDB('config').chunks.find({shard: st.shard0.shardName}).itcount());
    assert.eq(0, st.s0.getDB('config').chunks.find({shard: st.shard1.shardName}).itcount());

    unpauseMigrateAtStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

    // Ensure that migration succeeded
    joinMoveChunk();

    assert.eq(0, st.s0.getDB('config').chunks.find({shard: st.shard0.shardName}).itcount());
    assert.eq(1, st.s0.getDB('config').chunks.find({shard: st.shard1.shardName}).itcount());

    st.stop();
})();
