/**
 * Test that mongos getShardVersion returns the correct version and chunks.
 *
 * @tags: [need_fixing_for_46]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2, mongos: 1});
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const primaryShard = st.shard0;
const otherShard = st.shard1;
const min = {
    x: MinKey,
    y: MinKey
};
const max = {
    x: MaxKey,
    y: MaxKey
};

assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, primaryShard.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1, y: 1}}));

// Check shard version.
let res = st.s.adminCommand({getShardVersion: ns});
assert.commandWorked(res);
assert.eq(res.version.t, 1);
assert.eq(res.version.i, 0);
assert.eq(undefined, res.chunks);

// When fullMetadata set to true, chunks should be included in the response
// if the mongos version is v4.4.
res = st.s.adminCommand({getShardVersion: ns, fullMetadata: true});
assert.commandWorked(res);
assert.eq(res.version.t, 1);
assert.eq(res.version.i, 0);
assert.eq(1, res.chunks.length);
assert.eq(min, res.chunks[0][0]);
assert.eq(max, res.chunks[0][1]);

// Split the existing chunks to create a large number of chunks (> 16MB).
// This needs to be done twice since the BSONObj size limit also applies
// to command objects for commands like splitChunk.

// The chunk min and max need to be large, otherwise we need a lot more
// chunks to reach the size limit.
const splitPoint = {
    x: 0,
    y: "A".repeat(512)
};

let splitPoints = [];
for (let i = 0; i < 10000; i++) {
    splitPoints.push({x: i, y: splitPoint.y});
}
assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand({
    splitChunk: ns,
    from: st.shard0.shardName,
    min: min,
    max: max,
    keyPattern: {x: 1},
    splitKeys: splitPoints,
    epoch: res.versionEpoch,
}));

let prevMin = splitPoints[splitPoints.length - 1];
splitPoints = [];
for (let i = 10000; i < 20000; i++) {
    splitPoints.push({x: i, y: splitPoint.y});
}
assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand({
    splitChunk: ns,
    from: st.shard0.shardName,
    min: prevMin,
    max: max,
    keyPattern: {x: 1},
    splitKeys: splitPoints,
    epoch: res.versionEpoch,
}));

// Verify that moving a chunk won't trigger mongos's routing entry to get marked as stale until
// a request comes in to target that chunk.
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: splitPoint, to: otherShard.shardName}));

// Chunks should not be included in the response regardless of the mongos version
// because the chunk size exceeds the limit.
res = st.s.adminCommand({getShardVersion: ns, fullMetadata: true});
assert.commandWorked(res);
assert.eq(res.version.t, 3);
assert.eq(res.version.i, 10001);
assert.eq(undefined, res.chunks);

st.stop();
})();
