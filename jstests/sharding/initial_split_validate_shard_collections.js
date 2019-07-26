/**
 * Explicitly validates that all shards' collections have the correct UUIDs after an initial split
 * which spreads the collection across all available shards.
 */

load("jstests/libs/feature_compatibility_version.js");
load("jstests/libs/uuid_util.js");

(function() {
'use strict';

let st = new ShardingTest({shards: 2});
let mongos = st.s0;

assert.commandWorked(mongos.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard1.shardName);

assert.commandWorked(
    mongos.adminCommand({shardCollection: 'test.user', key: {x: 'hashed'}, numInitialChunks: 2}));

// Ensure that all the pending (received chunks) have been incorporated in the shard's filtering
// metadata so they will show up in the getShardVersion command
assert.eq(0, mongos.getDB('test').user.find({}).itcount());

st.printShardingStatus();

function checkMetadata(metadata) {
    jsTestLog(tojson(metadata));

    assert.eq(1, metadata.chunks.length);
    assert.eq(0, metadata.pending.length);

    // Check that the single chunk on the shard's metadata is a valid chunk (getShardVersion
    // represents chunks as an array of [min, max])
    let chunks = metadata.chunks;
    assert(bsonWoCompare(chunks[0][0], chunks[0][1]) < 0);
}

// Check that the shards' in-memory catalog caches were refreshed
checkMetadata(assert
                  .commandWorked(st.rs0.getPrimary().adminCommand(
                      {getShardVersion: 'test.user', fullMetadata: true}))
                  .metadata);
checkMetadata(assert
                  .commandWorked(st.rs1.getPrimary().adminCommand(
                      {getShardVersion: 'test.user', fullMetadata: true}))
                  .metadata);

// Check that the shards' catalogs have the correct UUIDs
const configUUID = getUUIDFromConfigCollections(mongos, 'test.user');
const shard0UUID = getUUIDFromListCollections(st.shard0.getDB('test'), 'user');
const shard1UUID = getUUIDFromListCollections(st.shard1.getDB('test'), 'user');
assert.eq(configUUID, shard0UUID);
assert.eq(configUUID, shard1UUID);

// Check that the shards' on-disk caches have the correct number of chunks
assert.commandWorked(
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: 'test.user', syncFromConfig: false}));
assert.commandWorked(
    st.shard1.adminCommand({_flushRoutingTableCacheUpdates: 'test.user', syncFromConfig: false}));

const chunksOnConfigCount = st.config.chunks.count({ns: 'test.user'});
assert.eq(2, chunksOnConfigCount);

const cacheChunksOnShard0 =
    st.shard0.getDB("config").getCollection("cache.chunks.test.user").find().toArray();
const cacheChunksOnShard1 =
    st.shard1.getDB("config").getCollection("cache.chunks.test.user").find().toArray();
assert.eq(chunksOnConfigCount, cacheChunksOnShard0.length);
assert.eq(chunksOnConfigCount, cacheChunksOnShard1.length);
assert.eq(cacheChunksOnShard0, cacheChunksOnShard1);

st.stop();
})();
