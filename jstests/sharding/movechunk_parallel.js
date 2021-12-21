/**
 * Ensures that two manual moveChunk commands for the same collection will proceed in parallel so
 * long as they do not touch the same shards
 *
 * @tags: [
 *   # SERVER-62181 avoid CS stepdowns, since they can cause the migrations
 *   # (in combination with their failpoints) issued by this test to enter a deadlock
 *   does_not_support_stepdowns,
 *  ]
 */

load('./jstests/libs/chunk_manipulation_util.js');
load("jstests/sharding/libs/find_chunks_util.js");

(function() {
'use strict';

// For startParallelOps to write its state
var staticMongod = MongoRunner.runMongod({});

var st = new ShardingTest({shards: 4});

assert.commandWorked(st.s0.adminCommand({enableSharding: 'TestDB'}));
st.ensurePrimaryShard('TestDB', st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: 'TestDB.TestColl', key: {Key: 1}}));

var coll = st.s0.getDB('TestDB').TestColl;

// Create 4 chunks initially
assert.commandWorked(coll.insert({Key: 1, Value: 'Test value 1'}));
assert.commandWorked(coll.insert({Key: 10, Value: 'Test value 10'}));
assert.commandWorked(coll.insert({Key: 20, Value: 'Test value 20'}));
assert.commandWorked(coll.insert({Key: 30, Value: 'Test value 30'}));

assert.commandWorked(st.splitAt('TestDB.TestColl', {Key: 10}));
assert.commandWorked(st.splitAt('TestDB.TestColl', {Key: 20}));
assert.commandWorked(st.splitAt('TestDB.TestColl', {Key: 30}));

// Move two of the chunks to st.shard1.shardName so we have option to do parallel balancing
assert.commandWorked(st.moveChunk('TestDB.TestColl', {Key: 20}, st.shard1.shardName));
assert.commandWorked(st.moveChunk('TestDB.TestColl', {Key: 30}, st.shard1.shardName));

assert.eq(
    2,
    findChunksUtil
        .findChunksByNs(st.s0.getDB('config'), 'TestDB.TestColl', {shard: st.shard0.shardName})
        .itcount());
assert.eq(
    2,
    findChunksUtil
        .findChunksByNs(st.s0.getDB('config'), 'TestDB.TestColl', {shard: st.shard1.shardName})
        .itcount());

// Pause migrations at shards 2 and 3
pauseMigrateAtStep(st.shard2, migrateStepNames.rangeDeletionTaskScheduled);
pauseMigrateAtStep(st.shard3, migrateStepNames.rangeDeletionTaskScheduled);

// Both move chunk operations should proceed
var joinMoveChunk1 = moveChunkParallel(
    staticMongod, st.s0.host, {Key: 10}, null, 'TestDB.TestColl', st.shard2.shardName);
var joinMoveChunk2 = moveChunkParallel(
    staticMongod, st.s0.host, {Key: 30}, null, 'TestDB.TestColl', st.shard3.shardName);

waitForMigrateStep(st.shard2, migrateStepNames.rangeDeletionTaskScheduled);
waitForMigrateStep(st.shard3, migrateStepNames.rangeDeletionTaskScheduled);

unpauseMigrateAtStep(st.shard2, migrateStepNames.rangeDeletionTaskScheduled);
unpauseMigrateAtStep(st.shard3, migrateStepNames.rangeDeletionTaskScheduled);

joinMoveChunk1();
joinMoveChunk2();

assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(st.s0.getDB('config'), 'TestDB.TestColl', {shard: st.shard0.shardName})
        .itcount());
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(st.s0.getDB('config'), 'TestDB.TestColl', {shard: st.shard1.shardName})
        .itcount());
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(st.s0.getDB('config'), 'TestDB.TestColl', {shard: st.shard2.shardName})
        .itcount());
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(st.s0.getDB('config'), 'TestDB.TestColl', {shard: st.shard3.shardName})
        .itcount());

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
