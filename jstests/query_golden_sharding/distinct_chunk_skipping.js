/**
 * Tests the results and explain output for multiplanning DISTINCT_SCANs in cases where the extra
 * work of embedded chunk skipping is evaluated during plan enumeration.
 *
 * TODO SERVER-101494: replace resource_intensive label and replace with a smaller burn_in job count.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   requires_fcv_82,
 *   resource_intensive,
 * ]
 */

import {section} from "jstests/libs/query/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckOrphans = true; // Deliberately inserts orphans.

// Disable range deletion to ensure predictable plan choice/ that orphans are where we think they should be.
const shardingTest = new ShardingTest({shards: 2, rs: {setParameter: {disableResumableRangeDeleter: true}}});
const db = shardingTest.getDB("test");

// Enable sharding.
const primaryShard = shardingTest.shard0.shardName;
const otherShard = shardingTest.shard1.shardName;
assert.commandWorked(shardingTest.s0.adminCommand({enableSharding: db.getName(), primaryShard}));

const coll = db[jsTestName()];
coll.drop();
coll.createIndex({a: 1, c: 1});
coll.createIndex({a: 1, b: 1, c: 1});

coll.insertMany([
    {a: "shard0_1", b: 1, c: 0},
    {a: "shard0_1", b: 2, c: 0},
    {a: "shard0_1", b: 2, c: 1},
    {a: "shard0_2", b: 3, c: 1},
    {a: "shard0_2", b: 1, c: 1},
    {a: "shard0_2", b: 1, c: 2},
    {a: "shard0_3", b: 2, c: 2},
    {a: "shard0_3", b: 3, c: 2},
    {a: "shard0_3", b: 3, c: 0},

    {a: "shard1_1", b: 1, c: 0},
    {a: "shard1_1", b: 2, c: 0},
    {a: "shard1_2", b: 3, c: 1},
    {a: "shard1_2", b: 1, c: 1},
    {a: "shard1_3", b: 2, c: 2},
    {a: "shard1_3", b: 3, c: 2},
]);

section("Unsharded environment uses DISTINCT_SCAN with embedded FETCH");
outputAggregationPlanAndResults(coll, [{$match: {a: {$gte: "shard0"}, c: {$eq: 1}}}, {$group: {_id: "$a"}}]);

// Move "shard_1*" chunk to "other" shard.
assert.commandWorked(shardingTest.s0.adminCommand({shardCollection: coll.getFullName(), key: {a: 1, b: 1, c: 1}}));
assert.commandWorked(shardingTest.s.adminCommand({split: coll.getFullName(), middle: {a: "shard1", b: 2, c: 1}}));
assert.commandWorked(
    shardingTest.s.adminCommand({moveChunk: coll.getFullName(), find: {a: "shard1_1", b: 2, c: 0}, to: otherShard}),
);

section("Selective query uses DISTINCT_SCAN + shard filtering + embedded FETCH, but no chunk skipping");
outputAggregationPlanAndResults(coll, [{$match: {a: {$gte: "shard0"}, c: {$eq: 1}}}, {$group: {_id: "$a"}}]);

section("Non-selective query uses DISTINCT_SCAN + shard filtering + embedded FETCH + chunk skipping");
outputAggregationPlanAndResults(coll, [{$match: {a: {$gte: "shard0"}, c: {$lte: 1}}}, {$group: {_id: "$a"}}]);

coll.createIndex({c: 1});
section("No DISTINCT_SCAN on 'a', shard filtering + FETCH + filter");
outputAggregationPlanAndResults(coll, [{$match: {a: {$gte: "shard0"}, c: {$eq: 1}}}, {$group: {_id: "$a"}}]);

// Re-enable the range deleter to drain during teardown.
[shardingTest.shard0, shardingTest.shard1].forEach((shard) => {
    assert.commandWorked(shard.adminCommand({setParameter: 1, disableResumableRangeDeleter: false}));
});

shardingTest.stop();
