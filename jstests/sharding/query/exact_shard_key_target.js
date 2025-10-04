//
// Verifies that shard key targeted update/delete operations go to exactly one shard when targeted
// by nested shard keys.
// SERVER-14138
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, verbose: 4});

let mongos = st.s0;
let coll = mongos.getCollection("foo.bar");
let admin = mongos.getDB("admin");

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {"a.b": 1}}));
assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {"a.b": 0}}));
assert.commandWorked(
    admin.runCommand({
        moveChunk: coll.getFullName(),
        find: {"a.b": 0},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

st.printShardingStatus();

//
// JustOne remove
coll.remove({});
assert.commandWorked(coll.insert({_id: 1, a: {b: -1}}));
assert.commandWorked(coll.insert({_id: 2, a: {b: 1}}));
let explainOutput = coll.explain().remove({a: {b: 1}}, {justOne: true});
assert.eq(1, explainOutput.queryPlanner.winningPlan.shards.length);

//
// Non-multi update
coll.remove({});
assert.commandWorked(coll.insert({_id: 1, a: {b: 1}}));
assert.commandWorked(coll.insert({_id: 2, a: {b: -1}}));
explainOutput = coll.explain().update({a: {b: 1}}, {$set: {updated: true}}, {multi: false});
assert.eq(1, explainOutput.queryPlanner.winningPlan.shards.length);

//
// Successive upserts (replacement-style)
coll.remove({});
assert.commandWorked(coll.update({a: {b: 1}}, {a: {b: 1}}, {upsert: true}));
assert.commandWorked(coll.update({a: {b: 1}}, {a: {b: 1}}, {upsert: true}));
assert.eq(1, st.shard0.getCollection(coll.toString()).count() + st.shard1.getCollection(coll.toString()).count());

//
// Successive upserts ($op-style)
coll.remove({});
assert.commandWorked(coll.update({a: {b: 1}}, {$set: {upserted: true}}, {upsert: true}));
assert.commandWorked(coll.update({a: {b: 1}}, {$set: {upserted: true}}, {upsert: true}));
assert.eq(1, st.shard0.getCollection(coll.toString()).count() + st.shard1.getCollection(coll.toString()).count());

jsTest.log("DONE!");
st.stop();
