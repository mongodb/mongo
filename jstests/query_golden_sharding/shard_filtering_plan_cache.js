/**
 * Tests the plan cache behavior for a DISTINCT_SCAN in case of a sharded collection where each
 * shard contains multiple (orphaned) chunks.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan
 * ]
 */

import {section} from "jstests/libs/pretty_md.js";
import {validateAggPlanCacheUse} from "jstests/libs/query/golden_test_utils.js";
import {setupShardedCollectionWithOrphans} from "jstests/libs/query_golden_sharding_utils.js";

TestData.skipCheckOrphans = true;  // Deliberately inserts orphans.
const {shardingTest, coll} = setupShardedCollectionWithOrphans();

coll.createIndex({shardKey: 1});
coll.createIndex({shardKey: 1, notShardKey: 1});

section("$group on shard key with $top/$bottom");
validateAggPlanCacheUse(
    coll,
    [{$group: {_id: "$shardKey", accum: {$top: {sortBy: {shardKey: 1}, output: "$shardKey"}}}}]);

section("$group on shard key with $first/$last");
validateAggPlanCacheUse(
    coll, [{$sort: {shardKey: -1}}, {$group: {_id: "$shardKey", accum: {$first: "$shardKey"}}}]);

validateAggPlanCacheUse(coll, [
    {$sort: {shardKey: 1, notShardKey: 1}},
    {$match: {shardKey: {$gt: "chunk1_s0"}}},
    {$group: {_id: "$shardKey", r: {$last: "$$ROOT"}}}
]);

shardingTest.stop();
