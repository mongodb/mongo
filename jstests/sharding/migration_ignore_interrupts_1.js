// When a migration is in progress from shard0 to shard1 on coll1, shard2 is unable to start a
// migration with either shard in the following cases:
//     1. coll2 shard0 to shard2 -- shard0 can't send two chunks simultaneously.
//     2. coll2 shard2 to shard1 -- shard1 can't receive two chunks simultaneously.

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    "use strict";

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({shards: 3});

    var mongos = st.s0, admin = mongos.getDB('admin'), dbName = "testDB", ns1 = dbName + ".foo",
        coll1 = mongos.getCollection(ns1), shard0 = st.shard0, shard1 = st.shard1,
        shard2 = st.shard2, shard0Coll1 = shard0.getCollection(ns1),
        shard1Coll1 = shard1.getCollection(ns1), shard2Coll1 = shard2.getCollection(ns1);

    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    assert.commandWorked(admin.runCommand({shardCollection: ns1, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: ns1, middle: {a: 0}}));
    assert.commandWorked(admin.runCommand({split: ns1, middle: {a: 10}}));
    assert.writeOK(coll1.insert({a: -10}));
    assert.writeOK(coll1.insert({a: 0}));
    assert.writeOK(coll1.insert({a: 10}));
    assert.eq(3, shard0Coll1.find().itcount());
    assert.eq(0, shard1Coll1.find().itcount());
    assert.eq(0, shard2Coll1.find().itcount());
    assert.eq(3, coll1.find().itcount());

    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 10}, to: st.shard2.shardName, _waitForDelete: true}));

    // Shard0:
    //      coll1:     [-inf, 0) [0, 10)
    // Shard1:
    // Shard2:
    //      coll1:     [10, +inf)

    jsTest.log("Set up complete, now proceeding to test that migration interruptions fail.");

    // Start a migration between shard0 and shard1 on coll1 and then pause it
    pauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMigrateStep(shard1, migrateStepNames.deletedPriorDataInRange);

    assert.commandFailedWithCode(
        admin.runCommand({moveChunk: ns1, find: {a: -10}, to: st.shard2.shardName}),
        ErrorCodes.ConflictingOperationInProgress,
        "(1) A shard should not be able to be the donor for two ongoing migrations.");

    assert.commandFailedWithCode(
        admin.runCommand({moveChunk: ns1, find: {a: 10}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress,
        "(2) A shard should not be able to be the recipient of two ongoing migrations.");

    assert.commandFailedWithCode(
        admin.runCommand({moveChunk: ns1, find: {a: 10}, to: st.shard0.shardName}),
        ErrorCodes.ConflictingOperationInProgress,
        "(3) A shard should not be able to be both a donor and recipient of migrations.");

    // Finish migration
    unpauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    assert.doesNotThrow(function() {
        joinMoveChunk();
    });
    assert.eq(1, shard0Coll1.find().itcount());
    assert.eq(1, shard1Coll1.find().itcount());
    assert.eq(1, shard2Coll1.find().itcount());

    st.stop();
})();
