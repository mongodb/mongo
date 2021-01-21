/**
 * Test that chunk operations preserve collection UUID in config.chunks documents
 */
// @tags: [multiversion_incompatible]

// TODO SERVER-53093 write unit tests checking for UUID in documents and throw out this test

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({mongos: 1, shards: 3});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
var collUUID;  // Initialized after shardCollection

{
    // Skip test if feature flag disabled
    let csrs_config_db = st.configRS.getPrimary().getDB('config');
    const isFeatureFlagEnabled =
        csrs_config_db
            .adminCommand({getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1})
            .featureFlagShardingFullDDLSupportTimestampedVersion.value;

    if (!isFeatureFlagEnabled) {
        st.stop();
        return;
    }
}

function allChunksWithUUID() {
    var cursor = findChunksUtil.findChunksByNs(st.config, ns);
    do {
        var next = cursor.next().uuid;
        assert.eq(collUUID, next);
    } while (cursor.hasNext());
}

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

collUUID = st.config.collections.findOne({_id: ns}).uuid;

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
