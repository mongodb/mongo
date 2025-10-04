// Remove operations leave orphaned documents.
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckOrphans = true;

// Test that idhack queries with projections obey the sharding filter.  SERVER-14032, SERVER-14034.

let st = new ShardingTest({shards: 2});
let coll = st.s0.getCollection("test.foo");

//
// Pre-split collection: shard 0 takes {x: {$lt: 0}}, shard 1 takes {x: {$gte: 0}}.
//
assert.commandWorked(
    coll.getDB().adminCommand({enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}),
);
assert.commandWorked(coll.getDB().adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
assert.commandWorked(coll.getDB().adminCommand({split: coll.getFullName(), middle: {x: 0}}));
assert.commandWorked(
    coll
        .getDB()
        .adminCommand({moveChunk: coll.getFullName(), find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
);

//
// Test that idhack queries with projections that remove the shard key return correct results.
// SERVER-14032.
//
assert.commandWorked(coll.insert({_id: 1, x: 1, y: 1}));
assert.eq(1, coll.find().itcount());
assert.eq(1, coll.find({_id: 1}, {x: 0}).itcount());
assert.eq(1, coll.find({_id: 1}, {y: 1}).itcount());
coll.remove({});

//
// Test that idhack queries with covered projections do not return orphan documents.  SERVER-14034.
//
assert.commandWorked(st.shard0.getCollection(coll.getFullName()).insert({_id: 1, x: 1}));
assert.commandWorked(st.shard1.getCollection(coll.getFullName()).insert({_id: 1, x: 1}));
assert.eq(2, coll.count());
assert.eq(1, coll.find().itcount());
assert.eq(1, coll.find({_id: 1}, {_id: 1}).itcount());
coll.remove({});

st.stop();
