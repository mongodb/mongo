/**
 * Test that chunk operations preserve collection UUID in config.chunks documents
 */

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({mongos: 1, shards: 3});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

function allChunksWithUUID() {
    const matchChunksWithoutUUID = {'uuid': null};
    assert.eq(0,
              st.config.chunks.countDocuments(matchChunksWithoutUUID),
              "Found chunks with wrong UUID " +
                  tojson(st.config.chunks.find(matchChunksWithoutUUID).toArray()));
}

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
allChunksWithUUID();

assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));
allChunksWithUUID();

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
allChunksWithUUID();

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
allChunksWithUUID();
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.shardName}));
allChunksWithUUID();

assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -500}}));
allChunksWithUUID();
assert.commandWorked(st.s.adminCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: -10}]}));
allChunksWithUUID();

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard1.shardName}));
allChunksWithUUID();
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
allChunksWithUUID();

st.stop();
})();
