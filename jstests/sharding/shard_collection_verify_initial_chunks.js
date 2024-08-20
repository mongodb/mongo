/**
 * Verify initial chunks are properly created and distributed in various combinations of shard key
 * and empty/non-empty collections.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 2});
let mongos = st.s0;
let db = mongos.getDB('TestDB');

assert.commandWorked(
    mongos.adminCommand({enableSharding: 'TestDB', primaryShard: st.shard1.shardName}));

function checkChunkCounts(collName, chunksOnShard0, chunksOnShard1) {
    let counts = st.chunkCounts(collName, 'TestDB');
    assert.eq(
        chunksOnShard0, counts[st.shard0.shardName], 'Count mismatch on shard0: ' + tojson(counts));
    assert.eq(
        chunksOnShard1, counts[st.shard1.shardName], 'Count mismatch on shard1: ' + tojson(counts));
}

let expectedNumberOfChunksPerShard = 1;
// TODO SERVER-81884: update once 8.0 becomes last LTS.
if (!FeatureFlagUtil.isPresentAndEnabled(mongos,
                                         "OneChunkPerShardEmptyCollectionWithHashedShardKey")) {
    expectedNumberOfChunksPerShard = 2;
}

// Hashed sharding + non-empty collection
assert.commandWorked(db.HashedCollNotEmpty.insert({aKey: 1}));
assert.commandWorked(db.HashedCollNotEmpty.createIndex({aKey: "hashed"}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: 'TestDB.HashedCollNotEmpty', key: {aKey: "hashed"}}));
assert.eq(1,
          findChunksUtil.countChunksForNs(st.config, 'TestDB.HashedCollNotEmpty'),
          "Count mismatch for total number of chunks");

// Hashed sharding + empty collection
assert.commandWorked(db.HashedCollEmpty.createIndex({aKey: "hashed"}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: 'TestDB.HashedCollEmpty', key: {aKey: "hashed"}}));
checkChunkCounts('HashedCollEmpty', expectedNumberOfChunksPerShard, expectedNumberOfChunksPerShard);

// Hashed sharding + non-existent collection
assert.commandWorked(
    mongos.adminCommand({shardCollection: 'TestDB.HashedCollNonExistent', key: {aKey: "hashed"}}));
checkChunkCounts(
    'HashedCollNonExistent', expectedNumberOfChunksPerShard, expectedNumberOfChunksPerShard);

st.stop();
