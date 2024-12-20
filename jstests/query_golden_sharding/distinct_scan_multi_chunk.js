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
import {setupShardedCollectionWithOrphans} from "jstests/libs/query_golden_sharding_utils.js";

TestData.skipCheckOrphans = true;  // Deliberately inserts orphans.

const {shardingTest, coll} = setupShardedCollectionWithOrphans();

section("distinct on shard key");
outputDistinctPlanAndResults(coll, "shardKey");
outputDistinctPlanAndResults(coll, "shardKey", {shardKey: {$eq: "chunk1_s0_1"}});
outputDistinctPlanAndResults(coll, "shardKey", {notShardKey: {$eq: "1notShardKey_chunk1_s0_1"}});
outputDistinctPlanAndResults(coll, "shardKey", {shardKey: {$gte: "chunk1_s0_1"}});
outputDistinctPlanAndResults(coll, "shardKey", {notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}});

coll.createIndex({notShardKey: 1});

section("distinct on non-shard key field");
outputDistinctPlanAndResults(coll, "notShardKey");
outputDistinctPlanAndResults(coll, "notShardKey", {shardKey: {$eq: "chunk1_s0_1"}});
outputDistinctPlanAndResults(coll, "notShardKey", {notShardKey: {$eq: "1notShardKey_chunk1_s0_1"}});
outputDistinctPlanAndResults(coll, "notShardKey", {shardKey: {$gte: "chunk1_s0_1"}});
outputDistinctPlanAndResults(
    coll, "notShardKey", {notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}});

section("$group on a non-shard key field");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$notShardKey"}}]);

subSection("$group on a non-shard key field with $first/$last");
outputAggregationPlanAndResults(coll,
                                [{$group: {_id: "$notShardKey", accum: {$first: "$notShardKey"}}}]);
outputAggregationPlanAndResults(coll,
                                [{$group: {_id: "$notShardKey", accum: {$first: "$shardKey"}}}]);
outputAggregationPlanAndResults(coll,
                                [{$group: {_id: "$notShardKey", accum: {$last: "$notShardKey"}}}]);
outputAggregationPlanAndResults(coll,
                                [{$group: {_id: "$notShardKey", accum: {$last: "$shardKey"}}}]);

subSection("$group on a non-shard key field with a preceding $match");
outputAggregationPlanAndResults(
    coll, [{$match: {shardKey: {$gte: "chunk1_s0_1"}}}, {$group: {_id: "$notShardKey"}}]);
outputAggregationPlanAndResults(
    coll,
    [{$match: {notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}}}, {$group: {_id: "$notShardKey"}}]);

subSection("$group on a non-shard key field with $first/$last a preceding $match");
outputAggregationPlanAndResults(coll, [
    {$match: {shardKey: {$gte: "chunk1_s0_1"}}},
    {$group: {_id: "$notShardKey", accum: {$first: "$notShardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$match: {shardKey: {$gte: "chunk1_s0_1"}}},
    {$group: {_id: "$notShardKey", accum: {$first: "$shardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$match: {notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}}},
    {$group: {_id: "$notShardKey", accum: {$last: "$notShardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$match: {notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}}},
    {$group: {_id: "$notShardKey", accum: {$last: "$shardKey"}}}
]);

subSection("$group on a non-shard key field with $first/$last and preceding $sort");
coll.createIndex({notShardKey: 1, shardKey: 1});
outputAggregationPlanAndResults(coll, [
    {$sort: {notShardKey: 1, shardKey: 1}},
    {$group: {_id: "$notShardKey", accum: {$first: "$notShardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {notShardKey: 1, shardKey: 1}},
    {$group: {_id: "$notShardKey", accum: {$first: "$shardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {notShardKey: 1, shardKey: 1}},
    {$group: {_id: "$notShardKey", accum: {$last: "$notShardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {notShardKey: 1, shardKey: 1}},
    {$group: {_id: "$notShardKey", accum: {$last: "$shardKey"}}}
]);

coll.dropIndex({notShardKey: 1, shardKey: 1});
coll.dropIndex({notShardKey: 1});

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
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$group: {_id: "$shardKey", accum: {$first: "$notShardKey"}}}
]);
outputAggregationPlanAndResults(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$group: {_id: "$shardKey", accum: {$last: "$notShardKey"}}}
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

section("distinct on multikey field");
coll.insertMany([{_id: "mk1", notShardKey: [1, 2, 3]}, {_id: "mk2", notShardKey: [2, 3, 4]}]);
coll.createIndex({notShardKey: 1});
coll.createIndex({notShardKey: 1, shardKey: 1});
// TODO SERVER-97235: See if we can avoid fetching here. If not, add a version of this test case
// without multikey values.
outputDistinctPlanAndResults(coll, "notShardKey");
outputDistinctPlanAndResults(coll, "notShardKey", {shardKey: null});
outputDistinctPlanAndResults(coll, "notShardKey", {notShardKey: 3});

shardingTest.stop();
