// Validates the remove/drain shard functionality when there is data on the shard being removed
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {moveOutSessionChunks, removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

let st = new ShardingTest({name: "remove_shard3", shards: 2, mongos: 2});

assert.commandWorked(st.s0.adminCommand({enableSharding: "TestDB", primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s0.adminCommand({shardCollection: "TestDB.Coll", key: {_id: 1}}));
assert.commandWorked(st.s0.adminCommand({split: "TestDB.Coll", middle: {_id: 0}}));

// Insert some documents and make sure there are docs on both shards
st.s0.getDB("TestDB").Coll.insert({_id: -1, value: "Negative value"});
st.s0.getDB("TestDB").Coll.insert({_id: 1, value: "Positive value"});

assert.commandWorked(
    st.s0.adminCommand({moveChunk: "TestDB.Coll", find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}),
);

// Make sure both mongos instances know of the latest metadata
assert.eq(2, st.s0.getDB("TestDB").Coll.find({}).toArray().length);
assert.eq(2, st.s1.getDB("TestDB").Coll.find({}).toArray().length);

// Remove st.shard1.shardName
let removeRes;
removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: st.shard1.shardName}));
assert.eq("started", removeRes.state);
removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: st.shard1.shardName}));
assert.eq("ongoing", removeRes.state);

// Move the one chunk off st.shard1.shardName
assert.commandWorked(
    st.s0.adminCommand({moveChunk: "TestDB.Coll", find: {_id: 1}, to: st.shard0.shardName, _waitForDelete: true}),
);

moveOutSessionChunks(st, st.shard1.shardName, st.shard0.shardName);

// Remove shard must succeed now
removeShard(st, st.shard1.shardName);

// Make sure both mongos instance refresh their metadata and do not reference the missing shard
assert.eq(2, st.s0.getDB("TestDB").Coll.find({}).toArray().length);
assert.eq(2, st.s1.getDB("TestDB").Coll.find({}).toArray().length);

st.stop();
