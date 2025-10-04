// This test ensures that if one collection is its migration critical section, this won't stall
// operations for other sharded or unsharded collections

import {
    moveChunkParallel,
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let staticMongod = MongoRunner.runMongod({});

let st = new ShardingTest({mongos: 1, shards: 2});
assert.commandWorked(st.s0.adminCommand({enableSharding: "TestDB", primaryShard: st.shard0.shardName}));

let testDB = st.s0.getDB("TestDB");

assert.commandWorked(st.s0.adminCommand({shardCollection: "TestDB.Coll0", key: {Key: 1}}));
assert.commandWorked(st.s0.adminCommand({split: "TestDB.Coll0", middle: {Key: 0}}));

let coll0 = testDB.Coll0;
assert.commandWorked(coll0.insert({Key: -1, Value: "-1"}));
assert.commandWorked(coll0.insert({Key: 1, Value: "1"}));

assert.commandWorked(st.s0.adminCommand({shardCollection: "TestDB.Coll1", key: {Key: 1}}));
assert.commandWorked(st.s0.adminCommand({split: "TestDB.Coll1", middle: {Key: 0}}));

let coll1 = testDB.Coll1;
assert.commandWorked(coll1.insert({Key: -1, Value: "-1"}));
assert.commandWorked(coll1.insert({Key: 1, Value: "1"}));

// Ensure that coll0 has chunks on both shards so we can test queries against both donor and
// recipient for Coll1's migration below
assert.commandWorked(st.s0.adminCommand({moveChunk: "TestDB.Coll0", find: {Key: 1}, to: st.shard1.shardName}));

// Pause the move chunk operation just before it leaves the critical section
pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

let joinMoveChunk = moveChunkParallel(staticMongod, st.s0.host, {Key: 1}, null, "TestDB.Coll1", st.shard1.shardName);

waitForMoveChunkStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

// Ensure that all operations for 'Coll0', which is not being migrated are not stalled
assert.eq(1, coll0.find({Key: {$lte: -1}}).itcount());
assert.eq(1, coll0.find({Key: {$gte: 1}}).itcount());
assert.commandWorked(coll0.insert({Key: -2, Value: "-2"}));
assert.commandWorked(coll0.insert({Key: 2, Value: "2"}));
assert.eq(2, coll0.find({Key: {$lte: -1}}).itcount());
assert.eq(2, coll0.find({Key: {$gte: 1}}).itcount());

// Ensure that read operations for 'Coll1', which *is* being migration are not stalled
assert.eq(1, coll1.find({Key: {$lte: -1}}).itcount());
assert.eq(1, coll1.find({Key: {$gte: 1}}).itcount());

// Ensure that all operations for non-sharded collections are not stalled
let collUnsharded = testDB.CollUnsharded;
assert.eq(0, collUnsharded.find({}).itcount());
assert.commandWorked(collUnsharded.insert({TestKey: 0, Value: "Zero"}));
assert.eq(1, collUnsharded.find({}).itcount());

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

joinMoveChunk();

st.stop();
MongoRunner.stopMongod(staticMongod);
