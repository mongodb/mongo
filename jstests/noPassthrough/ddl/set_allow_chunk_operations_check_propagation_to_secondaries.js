/**
 * Tests that DDLs installing authoritative metadata on shards correctly propagate the
 * allowChunkOperations flag to secondaries.
 *
 * @tags: [
 *  requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 3},
});

const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;
const dbName = "testdb";
const ns = dbName + ".coll";

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard1Name}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: 1}}));
assert.commandWorked(st.s.getCollection(ns).createIndex({a: 1, b: 1}));

// Run any DDL operation to broadcast setAllowChunkOperations to stop/resume chunk operations
// At this time, shard0 owns no chunks, so it should end up with a false value in allowChunkOperations
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: ns, key: {a: 1, b: 1}}));

// Force a stepdown-stepup to call _recoverAllowChunkOperations on shard0
st.rs0.stepUp(st.rs0.getSecondary());

// Move the fist chunk to shard0
assert.commandWorked(
    st.s.adminCommand({moveRange: ns, min: {a: 0, b: 0}, max: {a: 10, b: 0}, toShard: shard0Name}),
);

// A chunk operation on shard0 should not be rejected because allowChunkOperations is false
assert.commandWorked(st.s.adminCommand({split: ns, middle: {a: 5, b: 0}}));

st.stop();
