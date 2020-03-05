// Verifies refining a shard key checks for the presence of a compatible shard key index on a shard
// with chunks, not the primary shard.
// @tags: [requires_fcv_44]
(function() {
"use strict";

const st = new ShardingTest({shards: 2});

// The orphan hook assumes every shard has the shard key index, which is not true for test_primary
// after the refine.
TestData.skipCheckOrphans = true;

const dbName = "test_primary";
const collName = "foo";
const ns = dbName + "." + collName;

// Create a sharded collection with all chunks on the non-primary shard, shard1.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

// Move the last chunk away from the primary shard and create an index compatible with the refined
// key only on the non-primary shard.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));
assert.commandWorked(st.rs1.getPrimary().getCollection(ns).createIndex({x: 1, y: 1}));

// Refining the shard key should succeed even though the primary does not have a compatible index.
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: ns, key: {x: 1, y: 1}}));

st.stop();
})();
