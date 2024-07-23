/**
 * Tests the results and explain for a DISTINCT_SCAN in case of a sharded collection where each
 * shard contains multiple (orphaned) chunks.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */

import {line, outputDistinctPlanAndResults} from "jstests/libs/pretty_md.js";

TestData.skipCheckOrphans = true;  // Deliberately inserts orphans.
const shardingTest = new ShardingTest({shards: 2});

const db = shardingTest.getDB("test");

// Enable sharding.
const primaryShard = shardingTest.shard0.shardName;
const otherShard = shardingTest.shard1.shardName;
assert.commandWorked(shardingTest.s0.adminCommand({enableSharding: db.getName(), primaryShard}));

const shard0Chunks = ["chunk1_s0", "chunk2_s0", "chunk3_s0"];
const shard1Chunks = ["chunk1_s1", "chunk2_s1", "chunk3_s1"];
const allChunks = shard0Chunks.concat(shard1Chunks);

const coll = db[jsTestName()];
coll.drop();
coll.createIndex({shardKey: 1});

const docs = [];
for (const chunk of allChunks) {
    for (let i = 0; i < 3; i++) {
        docs.push({shardKey: `${chunk}_${i}`});
    }
}
coll.insertMany(docs);

assert.commandWorked(
    shardingTest.s0.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}));

// Split chunks up.
for (const chunk of allChunks) {
    assert.commandWorked(
        shardingTest.s.adminCommand({split: coll.getFullName(), middle: {shardKey: chunk}}));
}

// Move "shard 1" chunks off of primary.
for (const shardKey of shard1Chunks) {
    assert.commandWorked(shardingTest.s.adminCommand(
        {moveChunk: coll.getFullName(), find: {shardKey}, to: otherShard}));
}

{
    // Add orphans to primary.
    const docs = [];
    for (const chunk of shard1Chunks) {
        for (let i = 0; i < 3; i++) {
            docs.push({shardKey: `${chunk}_${i}_orphan`});
        }
    }
    assert.commandWorked(shardingTest.shard0.getCollection(coll.getFullName()).insert(docs));
}
{
    // Add orphans to secondary.
    const docs = [];
    for (const chunk of shard0Chunks) {
        for (let i = 0; i < 3; i++) {
            docs.push({shardKey: `${chunk}_${i}_orphan`});
        }
    }
    assert.commandWorked(shardingTest.shard1.getCollection(coll.getFullName()).insert(docs));
}

// Currently no sharding filter is applied on DISTINCT_SCAN plans.
line("TODO SERVER-92458: Shard filter out the orphan documents");
outputDistinctPlanAndResults(coll, "shardKey");

shardingTest.stop();
