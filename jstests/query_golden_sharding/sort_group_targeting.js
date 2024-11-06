/**
 * Tests the results and explain output for $sort+$group pipelines. The $group stage can in some
 * cases be pushed down to the shards.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */

import {outputAggregationPlanAndResults, section} from "jstests/libs/pretty_md.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const shardingTest = new ShardingTest({shards: 2});
const db = shardingTest.getDB("test");

// Enable sharding.
const primaryShard = shardingTest.shard0.shardName;
const otherShard = shardingTest.shard1.shardName;
assert.commandWorked(shardingTest.s0.adminCommand({enableSharding: db.getName(), primaryShard}));

const coll = db[jsTestName()];
coll.drop();
coll.createIndex({shardKey: 1});
coll.insertMany([
    {_id: 1, shardKey: "shard0_1", otherField: "a"},
    {_id: 2, shardKey: "shard0_2", otherField: "b"},
    {_id: 3, shardKey: "shard0_3", otherField: "c"},
    {_id: 4, shardKey: "shard1_1", otherField: "a"},
    {_id: 5, shardKey: "shard1_2", otherField: "b"},
    {_id: 6, shardKey: "shard1_3", otherField: "c"}
]);

section("$sort + $group for unsharded collection");
outputAggregationPlanAndResults(coll, [{$sort: {shardKey: 1}}, {$group: {_id: "$shardKey"}}]);

// Move "shard_1*" chunk to "other" shard.
assert.commandWorked(
    shardingTest.s0.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}));
assert.commandWorked(
    shardingTest.s.adminCommand({split: coll.getFullName(), middle: {shardKey: "shard1"}}));
assert.commandWorked(shardingTest.s.adminCommand(
    {moveChunk: coll.getFullName(), find: {shardKey: "shard1_1"}, to: otherShard}));

section("Push down $group preceded by $sort");
outputAggregationPlanAndResults(coll, [{$sort: {shardKey: 1}}, {$group: {_id: "$shardKey"}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {shardKey: 1}}, {$group: {_id: "$shardKey", first: {$first: "$otherField"}}}]);

// Note that this $group is still pushed down even though there's a $match between $sort & $group.
// This works because the pipeline is first optimized (where the $match is moved before the $sort)
// and then split.
outputAggregationPlanAndResults(coll, [
    {$project: {renamedShardKey: "$shardKey", otherField: 1}},
    {$sort: {renamedShardKey: 1}},
    {$match: {renamedShardKey: {$lte: "shard1_1"}}},
    {$group: {_id: "$renamedShardKey", last: {$last: "$otherField"}}}
]);

section("Don't push down $group preceded by $sort if $group is not on shard key");
outputAggregationPlanAndResults(coll, [{$sort: {shardKey: 1}}, {$group: {_id: "$otherField"}}]);

section("Don't push down $group preceded by $sort if the shard key is not preserved");
outputAggregationPlanAndResults(
    coll,
    [{$project: {shardKey: "$otherField"}}, {$sort: {shardKey: 1}}, {$group: {_id: "$shardKey"}}]);

shardingTest.stop();
