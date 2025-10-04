// Tests that save style updates correctly change immutable fields
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2, mongos: 1});

let mongos = st.s;
let config = mongos.getDB("config");
let coll = mongos.getCollection(jsTestName() + ".coll1");

assert.commandWorked(config.adminCommand({enableSharding: coll.getDB() + "", primaryShard: st.shard0.shardName}));
assert.commandWorked(config.adminCommand({shardCollection: "" + coll, key: {a: 1}}));

assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: coll.getFullName()}));

const shard0Coll = st.shard0.getCollection(coll.getFullName());

// Full shard key in save
assert.commandWorked(shard0Coll.save({_id: 1, a: 1}));

// Full shard key on replacement (basically the same as above)
shard0Coll.remove({});
assert.commandWorked(shard0Coll.update({_id: 1}, {a: 1}, true));

// Full shard key after $set
shard0Coll.remove({});
assert.commandWorked(shard0Coll.update({_id: 1}, {$set: {a: 1}}, true));

// Update existing doc (replacement), same shard key value
assert.commandWorked(shard0Coll.update({_id: 1}, {a: 1}));

// Update existing doc ($set), same shard key value
assert.commandWorked(shard0Coll.update({_id: 1}, {$set: {a: 1}}));

st.stop();
