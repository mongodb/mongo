/**
 * Tests the results and explain for a DISTINCT_SCAN in case of a sharded collection where each
 * shard contains multiple (orphaned) chunks.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan
 * ]
 */

import {
    outputAggregationPlanAndResults,
    outputDistinctPlanAndResults,
    section,
    subSection
} from "jstests/libs/pretty_md.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
coll.createIndex({shardKey: 1, notShardKey: 1});

const docs = [];
let _id = 0;  // We don't want non-deterministic _ids in $$ROOT tests.
for (const chunk of allChunks) {
    for (let i = 0; i < 3; i++) {
        docs.push(
            {_id: _id++, shardKey: `${chunk}_${i}`, notShardKey: `1notShardKey_${chunk}_${i}`},
            {_id: _id++, shardKey: `${chunk}_${i}`, notShardKey: `2notShardKey_${chunk}_${i}`},
            {_id: _id++, shardKey: `${chunk}_${i}`, notShardKey: `3notShardKey_${chunk}_${i}`});
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
            docs.push({
                shardKey: `${chunk}_${i}_orphan`,
                notShardKey: `notShardKey_${chunk}_${i}_orphan`
            });
        }
    }
    assert.commandWorked(shardingTest.shard0.getCollection(coll.getFullName()).insert(docs));
}
{
    // Add orphans to secondary.
    const docs = [];
    for (const chunk of shard0Chunks) {
        for (let i = 0; i < 3; i++) {
            docs.push({
                shardKey: `${chunk}_${i}_orphan`,
                notShardKey: `notShardKey_${chunk}_${i}_orphan`
            });
        }
    }
    assert.commandWorked(shardingTest.shard1.getCollection(coll.getFullName()).insert(docs));
}

section("distinct on shard key");
outputDistinctPlanAndResults(coll, "shardKey");

section("$group on shard key with $top/$bottom");
subSection("sort by shard key, output shard key");
outputAggregationPlanAndResults(
    coll,
    [{$group: {_id: "$shardKey", accum: {$top: {sortBy: {shardKey: 1}, output: "$shardKey"}}}}]);
outputAggregationPlanAndResults(
    coll,
    [{$group: {_id: "$shardKey", accum: {$top: {sortBy: {shardKey: -1}, output: "$shardKey"}}}}]);
outputAggregationPlanAndResults(
    coll,
    [{$group: {_id: "$shardKey", accum: {$bottom: {sortBy: {shardKey: 1}, output: "$shardKey"}}}}]);
outputAggregationPlanAndResults(coll, [
    {$group: {_id: "$shardKey", accum: {$bottom: {sortBy: {shardKey: -1}, output: "$shardKey"}}}}
]);

subSection("sort by shard key and another field, output shard key");
outputAggregationPlanAndResults(
    coll, [{
        $group: {
            _id: "$shardKey",
            accum: {$top: {sortBy: {shardKey: 1, notShardKey: 1}, output: "$shardKey"}}
        }
    }]);
outputAggregationPlanAndResults(
    coll, [{
        $group: {
            _id: "$shardKey",
            accum: {$bottom: {sortBy: {shardKey: 1, notShardKey: 1}, output: "$shardKey"}}
        }
    }]);

subSection("sort by shard key and another field, output non-shard key field");
outputAggregationPlanAndResults(
    coll, [{
        $group: {
            _id: "$shardKey",
            accum: {$top: {sortBy: {shardKey: 1, notShardKey: 1}, output: "$notShardKey"}}
        }
    }]);
outputAggregationPlanAndResults(
    coll, [{
        $group: {
            _id: "$shardKey",
            accum: {$bottom: {sortBy: {shardKey: 1, notShardKey: 1}, output: "$notShardKey"}}
        }
    }]);

// TODO SERVER-95198: Currently results in a COLLSCAN plan. However, in theory it could also be
// answered by a DISTINCT_SCAN on 'shardKey_1_notShardKey_1'.
subSection("sort by non-shard key field, output shard key");
outputAggregationPlanAndResults(
    coll,
    [{$group: {_id: "$shardKey", accum: {$top: {sortBy: {notShardKey: 1}, output: "$shardKey"}}}}]);
outputAggregationPlanAndResults(coll, [
    {$group: {_id: "$shardKey", accum: {$bottom: {sortBy: {notShardKey: 1}, output: "$shardKey"}}}}
]);

// TODO SERVER-95198: Currently results in a COLLSCAN plan. However, in theory it could also be
// answered by a DISTINCT_SCAN on 'shardKey_1_notShardKey_1'.
subSection("sort by non-shard key field, output non-shard key field");
outputAggregationPlanAndResults(coll, [
    {$group: {_id: "$shardKey", accum: {$top: {sortBy: {notShardKey: 1}, output: "$notShardKey"}}}}
]);
outputAggregationPlanAndResults(
    coll, [{
        $group:
            {_id: "$shardKey", accum: {$bottom: {sortBy: {notShardKey: 1}, output: "$notShardKey"}}}
    }]);

section("$group on shard key with $first/$last");
subSection("with preceding $sort on shard key");
outputAggregationPlanAndResults(
    coll, [{$sort: {shardKey: -1}}, {$group: {_id: "$shardKey", accum: {$first: "$shardKey"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {shardKey: 1}}, {$group: {_id: "$shardKey", accum: {$first: "$shardKey"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {shardKey: -1}}, {$group: {_id: "$shardKey", accum: {$last: "$shardKey"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {shardKey: 1}}, {$group: {_id: "$shardKey", accum: {$last: "$shardKey"}}}]);

subSection("with preceding $sort on shard key and another field");
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$group: {_id: "$shardKey", accum: {$first: "$shardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$group: {_id: "$shardKey", accum: {$last: "$shardKey"}}}
]);

subSection("without preceding $sort, output shard key");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$shardKey", accum: {$first: "$shardKey"}}}]);
outputAggregationPlanAndResults(coll, [{$group: {_id: "$shardKey", accum: {$last: "$shardKey"}}}]);

subSection("with preceding $sort and intervening $match");
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$match: {shardKey: {$gt: "chunk1_s0"}}},
    {$group: {_id: "$shardKey", l1: {$first: "$shardKey"}, l2: {$first: "$notShardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$match: {shardKey: {$gt: "chunk1_s0"}}},
    {$group: {_id: "$shardKey", l1: {$last: "$shardKey"}, l2: {$last: "$notShardKey"}}}
]);

subSection("without preceding $sort, output non-shard key field");
outputAggregationPlanAndResults(coll,
                                [{$group: {_id: "$shardKey", accum: {$first: "$notShardKey"}}}]);
outputAggregationPlanAndResults(coll,
                                [{$group: {_id: "$shardKey", accum: {$last: "$notShardKey"}}}]);

subSection("with preceding $sort and intervening $match, output non-shard key field");
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$match: {shardKey: {$gt: "chunk1_s0"}}},
    {$group: {_id: "$shardKey", r: {$first: "$$ROOT"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$match: {shardKey: {$gt: "chunk1_s0"}}},
    {$group: {_id: "$shardKey", r: {$last: "$$ROOT"}}}
]);

shardingTest.stop();
