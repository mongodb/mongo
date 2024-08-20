/**
 * Explicitly validates that all shards' collections have the correct UUIDs after an initial split
 * which spreads the collection across all available shards.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 2});
let mongos = st.s0;

assert.commandWorked(
    mongos.adminCommand({enableSharding: 'test', primaryShard: st.shard1.shardName}));

assert.commandWorked(mongos.adminCommand({shardCollection: 'test.user', key: {x: 'hashed'}}));

// Ensure that all the pending (received chunks) have been incorporated in the shard's filtering
// metadata so they will show up in the getShardVersion command
assert.eq(0, mongos.getDB('test').user.find({}).itcount());

let expectedChunksOnConfigCount = 2;
let expectedChunksPerShardCount = 1;
// TODO SERVER-81884: update once 8.0 becomes last LTS.
if (!FeatureFlagUtil.isPresentAndEnabled(mongos,
                                         "OneChunkPerShardEmptyCollectionWithHashedShardKey")) {
    expectedChunksOnConfigCount = 4;
    expectedChunksPerShardCount = 2;
}

st.printShardingStatus();

function checkMetadata(metadata) {
    jsTestLog(tojson(metadata));

    assert.eq(expectedChunksPerShardCount, metadata.chunks.length);

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
const collEntry = mongos.getDB("config").collections.findOne({_id: 'test.user'});
const shard0UUID = getUUIDFromListCollections(st.shard0.getDB('test'), 'user');
const shard1UUID = getUUIDFromListCollections(st.shard1.getDB('test'), 'user');
assert.eq(collEntry.uuid, shard0UUID);
assert.eq(collEntry.uuid, shard1UUID);

// Check that the shards' on-disk caches have the correct number of chunks
assert.commandWorked(
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: 'test.user', syncFromConfig: false}));
assert.commandWorked(
    st.shard1.adminCommand({_flushRoutingTableCacheUpdates: 'test.user', syncFromConfig: false}));

const chunksOnConfigCount = findChunksUtil.countChunksForNs(st.config, 'test.user');
assert.eq(expectedChunksOnConfigCount, chunksOnConfigCount);

const chunksCollName = "cache.chunks.test.user";
const chunksOnShard0 = st.shard0.getDB("config").getCollection(chunksCollName).find().toArray();
const chunksOnShard1 = st.shard1.getDB("config").getCollection(chunksCollName).find().toArray();
assert.eq(chunksOnConfigCount, chunksOnShard0.length);
assert.eq(chunksOnConfigCount, chunksOnShard1.length);

st.stop();
