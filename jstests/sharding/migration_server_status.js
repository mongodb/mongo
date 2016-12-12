//
// Tests that serverStatus includes a migration status when called on the source shard of an active
// migration.
//

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    'use strict';

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var coll = mongos.getCollection("db.coll");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    st.ensurePrimaryShard(coll.getDB() + "", st.shard0.shardName);
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

    // Pause the migration once it starts on both shards -- somewhat arbitrary pause point.
    pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.startedMoveChunk);

    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {_id: 1}, null, coll.getFullName(), st.shard1.shardName);

    var assertMigrationStatusOnServerStatus = function(serverStatusResult,
                                                       sourceShard,
                                                       destinationShard,
                                                       isDonorShard,
                                                       minKey,
                                                       maxKey,
                                                       collectionName) {
        var migrationResult = serverStatusResult.sharding.migrations;
        assert.eq(sourceShard, migrationResult.source);
        assert.eq(destinationShard, migrationResult.destination);
        assert.eq(isDonorShard, migrationResult.isDonorShard);
        assert.eq(minKey, migrationResult.chunk.min);
        assert.eq(maxKey, migrationResult.chunk.max);
        assert.eq(collectionName, migrationResult.collection);
    };

    waitForMoveChunkStep(st.shard0, moveChunkStepNames.startedMoveChunk);

    // Source shard should return a migration status.
    var shard0ServerStatus = st.shard0.getDB('admin').runCommand({serverStatus: 1});
    assert(shard0ServerStatus.sharding.migrations);
    assertMigrationStatusOnServerStatus(shard0ServerStatus,
                                        st.shard0.shardName,
                                        st.shard1.shardName,
                                        true,
                                        {"_id": 0},
                                        {"_id": {"$maxKey": 1}},
                                        coll + "");

    // Destination shard should not return any migration status.
    var shard1ServerStatus = st.shard1.getDB('admin').runCommand({serverStatus: 1});
    assert(!shard1ServerStatus.sharding.migrations);

    // Mongos should never return a migration status.
    var mongosServerStatus = st.s0.getDB('admin').runCommand({serverStatus: 1});
    assert(!mongosServerStatus.sharding.migrations);

    unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.startedMoveChunk);
    joinMoveChunk();

    // Migration is over, should no longer get a migration status.
    var shard0ServerStatus = st.shard0.getDB('admin').runCommand({serverStatus: 1});
    assert(!shard0ServerStatus.sharding.migrations);

    st.stop();

})();
