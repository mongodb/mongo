//
// These tests validates that a migration with session IDs between two shards is protected
// against disruptive migration commands from a third shard. It tests several scenarios.
// For more information on migration session IDs see SERVER-20290.
//

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    "use strict";

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // Starting setup
    ///////////////////////////////////////////////////////////////////////////////////////////////////

    // Shard0:
    //      coll1:     [0, 10) [10, 20) [20, 30)
    //      coll2:     [0, 10) [10, 20)
    // Shard1:
    // Shard2:

    var staticMongod1 = MongoRunner.runMongod({});  // For startParallelOps.
    var staticMongod2 = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({shards: 4, mongos: 1});

    var mongos = st.s0, admin = mongos.getDB('admin'), dbName = "testDB", ns1 = dbName + ".foo",
        coll1 = mongos.getCollection(ns1), ns2 = dbName + ".baz", coll2 = mongos.getCollection(ns2),
        shard0 = st.shard0, shard1 = st.shard1, shard2 = st.shard2,
        shard0Coll1 = shard0.getCollection(ns1), shard0Coll2 = shard0.getCollection(ns2),
        shard1Coll1 = shard1.getCollection(ns1), shard1Coll2 = shard1.getCollection(ns2),
        shard2Coll1 = shard2.getCollection(ns1), shard2Coll2 = shard2.getCollection(ns2);

    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    assert.commandWorked(admin.runCommand({shardCollection: ns1, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: ns1, middle: {a: 10}}));
    assert.commandWorked(admin.runCommand({split: ns1, middle: {a: 20}}));
    assert.commandWorked(admin.runCommand({shardCollection: ns2, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: ns2, middle: {a: 10}}));

    assert.writeOK(coll1.insert({a: 0}));
    assert.writeOK(coll1.insert({a: 10}));
    assert.writeOK(coll1.insert({a: 20}));
    assert.eq(3, shard0Coll1.count());
    assert.eq(3, coll1.count());
    assert.writeOK(coll2.insert({a: 0}));
    assert.writeOK(coll2.insert({a: 10}));
    assert.eq(2, shard0Coll2.count());
    assert.eq(2, coll2.count());

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //      1. When a migration is in process from shard0 to shard1 on coll1, shard2 is unable to
    //         start a migration with either shard in the following cases:
    //               1. coll1 shard2 to shard0 -- coll1 is already locked.
    //               2. coll1 shard2 to shard1 -- coll1 is already locked.
    //               3. coll1 shard1 to shard2 -- coll1 is already locked.
    //               4. coll2 shard2 to shard1 -- shard1 can't receive two chunks simultaneously.
    //               5. coll2 shard0 to shard2 -- shard0 can't send two chunks simultaneously.
    ///////////////////////////////////////////////////////////////////////////////////////////////////

    // Shard0:
    //      coll1:     [0, 10)
    //      coll2:     [0, 10)
    // Shard1:
    //      coll1:     [20, 30)
    // Shard2:
    //      coll1:     [10, 20)
    //      coll2:     [10, 20)

    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 10}, to: st.shard2.shardName, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns2, find: {a: 10}, to: st.shard2.shardName, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 20}, to: st.shard1.shardName, _waitForDelete: true}));
    assert.eq(1, shard0Coll1.count());
    assert.eq(1, shard0Coll2.count());
    assert.eq(1, shard1Coll1.count());
    assert.eq(0, shard1Coll2.count());
    assert.eq(1, shard2Coll1.count());
    assert.eq(1, shard2Coll2.count());

    // Start a migration between shard0 and shard1 on coll1 and then pause it
    pauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    var joinMoveChunk1 = moveChunkParallel(
        staticMongod1, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMigrateStep(shard1, migrateStepNames.deletedPriorDataInRange);

    jsTest.log('Attempting to interrupt migration....');
    // Test 1.1
    assert.commandFailed(
        admin.runCommand({moveChunk: ns1, find: {a: 10}, to: st.shard0.shardName}),
        "(1.1) coll1 lock should have prevented simultaneous migrations in the collection.");
    // Test 1.2
    assert.commandFailed(
        admin.runCommand({moveChunk: ns1, find: {a: 10}, to: st.shard1.shardName}),
        "(1.2) coll1 lock should have prevented simultaneous migrations in the collection.");
    // Test 1.3
    assert.commandFailed(
        admin.runCommand({moveChunk: ns1, find: {a: 20}, to: st.shard2.shardName}),
        "(1.3) coll1 lock should have prevented simultaneous migrations in the collection.");
    // Test 1.4
    assert.commandFailed(
        admin.runCommand({moveChunk: ns2, find: {a: 10}, to: st.shard1.shardName}),
        "(1.4) A shard should not be able to be the recipient of two ongoing migrations");
    // Test 1.5
    assert.commandFailed(
        admin.runCommand({moveChunk: ns2, find: {a: 0}, to: st.shard2.shardName}),
        "(1.5) A shard should not be able to be the donor for two ongoing migrations.");

    // Finish migration
    unpauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    assert.doesNotThrow(function() {
        joinMoveChunk1();
    });
    assert.eq(0, shard0Coll1.count());
    assert.eq(2, shard1Coll1.count());

    // Reset setup
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 0}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 20}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 10}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns2, find: {a: 10}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.eq(3, shard0Coll1.count());
    assert.eq(2, shard0Coll2.count());
    assert.eq(0, shard1Coll1.count());
    assert.eq(0, shard1Coll2.count());
    assert.eq(0, shard2Coll1.count());
    assert.eq(0, shard2Coll2.count());

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //      2. When a migration between shard0 and shard1 is about to enter the commit phase, a
    //         commit command from shard2 (different migration session ID) is rejected.
    ///////////////////////////////////////////////////////////////////////////////////////////////////

    // Shard0:
    //      coll1:     [0, 10) [10, 20) [20, 30)
    //      coll2:     [0, 10) [10, 20)
    // Shard1:
    // Shard2:

    // Start a migration between shard0 and shard1 on coll1, pause in steady state before commit
    pauseMoveChunkAtStep(shard0, moveChunkStepNames.reachedSteadyState);
    joinMoveChunk1 = moveChunkParallel(
        staticMongod1, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMoveChunkStep(shard0, moveChunkStepNames.reachedSteadyState);

    jsTest.log('Sending false commit command....');
    assert.commandFailed(
        shard2.adminCommand({'_recvChunkCommit': 1, 'sessionId': "fake-migration-session-id"}));

    jsTest.log("Checking migration recipient is still in steady state, waiting for commit....");
    var res = shard1.adminCommand('_recvChunkStatus');
    assert.commandWorked(res);
    assert.eq(true, res.state === "steady", "False commit command succeeded");

    // Finish migration
    unpauseMoveChunkAtStep(shard0, moveChunkStepNames.reachedSteadyState);
    assert.doesNotThrow(function() {
        joinMoveChunk1();
    });
    assert.eq(2, shard0Coll1.count());
    assert.eq(1, shard1Coll1.count());

    // Reset setup
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns1, find: {a: 0}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.eq(3, shard0Coll1.count());
    assert.eq(2, shard0Coll2.count());
    assert.eq(0, shard1Coll1.count());
    assert.eq(0, shard1Coll2.count());
    assert.eq(0, shard2Coll1.count());
    assert.eq(0, shard2Coll2.count());

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //      3. If a donor aborts a migration to a recipient, the recipient does not realize the
    //         migration has been aborted, and the donor moves on to a new migration, the original
    //         recipient will then fail to clone documents from the donor.
    ///////////////////////////////////////////////////////////////////////////////////////////////////

    // Shard0:
    //      coll1:     [0, 10) [10, 20) [20, 30)
    //      coll2:     [0, 10) [10, 20)
    // Shard1:
    // Shard2:

    // Start coll1 migration to shard1: pause recipient after delete step, donor before interrupt
    // check
    pauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    pauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
    joinMoveChunk1 = moveChunkParallel(
        staticMongod1, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMigrateStep(shard1, migrateStepNames.deletedPriorDataInRange);

    // Abort migration on donor side, recipient is unaware
    var inProgressOps = admin.currentOp().inprog;
    var abortedMigration = false;
    for (var op in inProgressOps) {
        if (inProgressOps[op].query.moveChunk) {
            admin.killOp(inProgressOps[op].opid);
            abortedMigration = true;
        }
    }
    assert.eq(true,
              abortedMigration,
              "Failed to abort migration, current running ops: " + tojson(inProgressOps));
    unpauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
    assert.throws(function() {
        joinMoveChunk1();
    });

    // Start coll2 migration to shard2, pause recipient after delete step
    pauseMigrateAtStep(shard2, migrateStepNames.deletedPriorDataInRange);
    var joinMoveChunk2 = moveChunkParallel(
        staticMongod2, st.s0.host, {a: 0}, null, coll2.getFullName(), st.shard2.shardName);
    waitForMigrateStep(shard2, migrateStepNames.deletedPriorDataInRange);

    jsTest.log('Releasing coll1 migration recipient, whose clone command should fail....');
    unpauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    assert.eq(3, shard0Coll1.count(), "donor shard0 completed a migration that it aborted");
    assert.eq(0, shard1Coll1.count(), "shard1 cloned documents despite donor migration abortion");

    jsTest.log('Finishing coll2 migration, which should succeed....');
    unpauseMigrateAtStep(shard2, migrateStepNames.deletedPriorDataInRange);
    assert.doesNotThrow(function() {
        joinMoveChunk2();
    });
    assert.eq(1,
              shard0Coll2.count(),
              "donor shard0 failed to complete a migration " + "after aborting a prior migration");
    assert.eq(1, shard2Coll2.count(), "shard2 failed to complete migration");

    // Reset setup
    assert.commandWorked(admin.runCommand(
        {moveChunk: ns2, find: {a: 0}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.eq(3, shard0Coll1.count());
    assert.eq(2, shard0Coll2.count());
    assert.eq(0, shard1Coll1.count());
    assert.eq(0, shard1Coll2.count());
    assert.eq(0, shard2Coll1.count());
    assert.eq(0, shard2Coll2.count());

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    //      4. If a donor aborts a migration to a recipient, the recipient does not realize the
    //         migration has been aborted, and the donor moves on to a new migration, the original
    //         recipient will then fail to retrieve transferMods from the donor's xfermods log.
    ///////////////////////////////////////////////////////////////////////////////////////////////////

    // Shard0:
    //      coll1:     [0, 10) [10, 20) [20, 30)
    //      coll2:     [0, 10) [10, 20)
    // Shard1:
    // Shard2:

    // Start coll1 migration to shard1: pause recipient after cloning, donor before interrupt check
    pauseMigrateAtStep(shard1, migrateStepNames.cloned);
    pauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
    joinMoveChunk1 = moveChunkParallel(
        staticMongod1, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMigrateStep(shard1, migrateStepNames.cloned);

    // Abort migration on donor side, recipient is unaware
    inProgressOps = admin.currentOp().inprog;
    abortedMigration = false;
    for (var op in inProgressOps) {
        if (inProgressOps[op].query.moveChunk) {
            admin.killOp(inProgressOps[op].opid);
            abortedMigration = true;
        }
    }
    assert.eq(true,
              abortedMigration,
              "Failed to abort migration, current running ops: " + tojson(inProgressOps));
    unpauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
    assert.throws(function() {
        joinMoveChunk1();
    });

    // Start coll2 migration to shard2, pause recipient after cloning step
    pauseMigrateAtStep(shard2, migrateStepNames.cloned);
    var joinMoveChunk2 = moveChunkParallel(
        staticMongod2, st.s0.host, {a: 0}, null, coll2.getFullName(), st.shard2.shardName);
    waitForMigrateStep(shard2, migrateStepNames.cloned);

    // Populate donor (shard0) xfermods log.
    assert.writeOK(coll2.insert({a: 1}));
    assert.writeOK(coll2.insert({a: 2}));
    assert.eq(4, coll2.count(), "Failed to insert documents into coll2");
    assert.eq(4, shard0Coll2.count());

    jsTest.log('Releasing coll1 migration recipient, whose transferMods command should fail....');
    unpauseMigrateAtStep(shard1, migrateStepNames.cloned);
    assert.eq(3, shard0Coll1.count(), "donor shard0 completed a migration that it aborted");
    assert.eq(1,
              shard1Coll1.count(),
              "shard1 accessed the xfermods log despite " + "donor migration abortion");

    jsTest.log('Finishing coll2 migration, which should succeed....');
    unpauseMigrateAtStep(shard2, migrateStepNames.cloned);
    assert.doesNotThrow(function() {
        joinMoveChunk2();
    });
    assert.eq(1,
              shard0Coll2.count(),
              "donor shard0 failed to complete a migration " + "after aborting a prior migration");
    assert.eq(3, shard2Coll2.count(), "shard2 failed to complete migration");

    st.stop();

})();
