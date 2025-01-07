/*
 * Ensures that $unionWith stage explain code properly refreshes all the intermediary shards cache.
 */
const st = new ShardingTest({shards: 3});

// Create unsharded untracked collections in shard0.
assert.commandWorked(
    st.s.adminCommand({enableSharding: 'union_with', primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.getDB('union_with').createCollection('A'));
assert.commandWorked(st.s.getDB('union_with').createCollection('B'));

assert.commandWorked(
    st.s.adminCommand({moveCollection: 'union_with.A', toShard: st.shard2.shardName}));

// Ensure that cache on shard2 of collection B is refreshed, marking it as unsharded.
assert.commandWorked(st.s.getDB('union_with')
                         .getCollection('A')
                         .aggregate([{$unionWith: {coll: 'B', pipeline: []}}], {explain: true}));

// Move collection B to shard1, leaving the cache in shard2 stale, considering moveCollection() will
// only refresh shard0 and shard1.
assert.commandWorked(
    st.s.adminCommand({moveCollection: 'union_with.B', toShard: st.shard1.shardName}));

// Ensure that shard2 refreshes its cache and routes the command correctly.
assert.commandWorked(st.s.getDB('union_with')
                         .getCollection('A')
                         .aggregate([{$unionWith: {coll: 'B', pipeline: []}}], {explain: true}));

st.stop();
