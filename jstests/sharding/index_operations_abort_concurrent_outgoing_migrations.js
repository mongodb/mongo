/*
 * Test that the index commands abort concurrent outgoing migrations.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load('jstests/libs/chunk_manipulation_util.js');
load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/sharded_index_util.js");

/*
 * Runs moveChunk on the host to move the chunk to the given shard.
 */
function runMoveChunk(host, ns, findCriteria, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveChunk: ns, find: findCriteria, to: toShard});
}

/*
 * Runs moveChunk to move the initial chunk from the primary shard (shard0) to shard1. Pauses
 * the migration at the given step and runs the given command function. Asserts that the command
 * aborts the outgoing migration.
 */
function assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, cmdFunc) {
    const fromShard = st.shard0;
    const toShard = st.shard1;

    // Turn on the fail point and wait for moveChunk to hit the fail point.
    pauseMoveChunkAtStep(fromShard, stepName);
    let moveChunkThread = new Thread(runMoveChunk, st.s.host, ns, {_id: MinKey}, toShard.shardName);
    moveChunkThread.start();
    waitForMoveChunkStep(fromShard, stepName);

    cmdFunc();

    // Turn off the fail point and wait for moveChunk to complete.
    unpauseMoveChunkAtStep(fromShard, stepName);
    moveChunkThread.join();
    assert.commandFailedWithCode(moveChunkThread.returnData(), ErrorCodes.Interrupted);
}

const st = new ShardingTest({shards: 2});
const dbName = "test";
const testDB = st.s.getDB(dbName);
const shardKey = {
    _id: 1
};
const index = {
    x: 1
};
// The steps after cloning starts and before the donor enters the critical section.
const stepNames = [moveChunkStepNames.startedMoveChunk, moveChunkStepNames.reachedSteadyState];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

stepNames.forEach((stepName) => {
    jsTest.log(`Testing that createIndexes aborts concurrent outgoing migrations that are in step ${
        stepName}...`);
    const collName = "testCreateIndexesMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        assert.commandWorked(st.s.getCollection(ns).createIndexes([index]));

        // Verify that the index command succeeds.
        ShardedIndexUtil.assertIndexExistsOnShard(st.shard0, dbName, collName, index);
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard1, dbName, collName, index);
    });
});

stepNames.forEach((stepName) => {
    jsTest.log(`Testing that dropIndexes aborts concurrent outgoing migrations that are in step ${
        stepName}...`);
    const collName = "testDropIndexesMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    // Create the index on the primary shard prior to the migration so that the migration is not
    // aborted because of createIndexes instead of dropIndexes.
    assert.commandWorked(st.shard0.getCollection(ns).createIndexes([index]));

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        assert.commandWorked(st.s.getCollection(ns).dropIndexes(index));

        // Verify that the index command succeeds.
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard0, dbName, collName, index);
        ShardedIndexUtil.assertIndexExistsOnShard(st.shard1, dbName, collName, index);
    });
});

stepNames.forEach((stepName) => {
    jsTest.log(`Testing that collMod aborts concurrent outgoing migrations that are in step ${
        stepName}...`);
    const collName = "testCollModMoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        assert.commandWorked(
            testDB.runCommand({collMod: collName, validator: {x: {$type: "string"}}}));

        // Verify that the index command succeeds.
        assert.commandFailedWithCode(st.shard0.getCollection(ns).insert({x: 1}),
                                     ErrorCodes.DocumentValidationFailure);
        assert.commandWorked(st.shard1.getCollection(ns).insert({x: 1}));
    });
});

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

stepNames.forEach((stepName) => {
    jsTest.log(
        `Testing that createIndexes in FCV 4.2 aborts concurrent outgoing migrations that are in step ${
            stepName}...`);
    const collName = "testCreateIndexesFCV42MoveChunkStep" + stepName;
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));

    assertCommandAbortsConcurrentOutgoingMigration(st, stepName, ns, () => {
        assert.commandWorked(st.s.getCollection(ns).createIndexes([index]));

        // Verify that the index command succeeds.
        ShardedIndexUtil.assertIndexExistsOnShard(st.shard0, dbName, collName, index);
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard1, dbName, collName, index);
    });
});

st.stop();
})();
