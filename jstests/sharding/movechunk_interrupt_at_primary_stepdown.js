// Ensures that all pending move chunk operations get interrupted when the primary of the config
// server steps down and then becomes primary again. Then the migration can be rejoined, and a
// success/failure response still returned to the caller.
//
// Also tests the failure of a migration commit command on the source shard of a migration, due to
// the balancer being interrupted, failing to recover the active migrations, and releasing the
// distributed lock.

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    'use strict';

    // Intentionally use a config server with 1 node so that the step down and promotion to primary
    // are guaranteed to happen on the same host
    var st = new ShardingTest({config: 1, shards: 2});
    var mongos = st.s0;

    assert.commandWorked(mongos.adminCommand({enableSharding: 'TestDB'}));
    st.ensurePrimaryShard('TestDB', st.shard0.shardName);
    assert.commandWorked(mongos.adminCommand({shardCollection: 'TestDB.TestColl', key: {Key: 1}}));

    var coll = mongos.getDB('TestDB').TestColl;

    // We have one chunk initially
    assert.writeOK(coll.insert({Key: 0, Value: 'Test value'}));

    pauseMigrateAtStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

    // For startParallelOps to write its state
    var staticMongod = MongoRunner.runMongod({});

    var joinMoveChunk = moveChunkParallel(
        staticMongod, mongos.host, {Key: 0}, null, 'TestDB.TestColl', st.shard1.shardName);
    waitForMigrateStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

    // Stepdown the primary in order to force the balancer to stop. Use a timeout of 5 seconds for
    // both step down operations, because mongos will retry to find the CSRS primary for up to 20
    // seconds and we have two successive ones.
    assert.throws(function() {
        assert.commandWorked(
            st.configRS.getPrimary().adminCommand({replSetStepDown: 5, force: true}));
    });

    // Ensure a new primary is found promptly
    st.configRS.getPrimary(30000);

    assert.eq(1,
              mongos.getDB('config')
                  .chunks.find({ns: 'TestDB.TestColl', shard: st.shard0.shardName})
                  .itcount());
    assert.eq(0,
              mongos.getDB('config')
                  .chunks.find({ns: 'TestDB.TestColl', shard: st.shard1.shardName})
                  .itcount());

    // At this point, the balancer is in recovery mode. Ensure that stepdown can be done again and
    // the recovery mode interrupted.
    assert.throws(function() {
        assert.commandWorked(
            st.configRS.getPrimary().adminCommand({replSetStepDown: 5, force: true}));
    });

    // Ensure a new primary is found promptly
    st.configRS.getPrimary(30000);

    unpauseMigrateAtStep(st.shard1, migrateStepNames.deletedPriorDataInRange);

    // Ensure that migration succeeded
    joinMoveChunk();

    assert.eq(0,
              mongos.getDB('config')
                  .chunks.find({ns: 'TestDB.TestColl', shard: st.shard0.shardName})
                  .itcount());
    assert.eq(1,
              mongos.getDB('config')
                  .chunks.find({ns: 'TestDB.TestColl', shard: st.shard1.shardName})
                  .itcount());

    st.stop();
})();
