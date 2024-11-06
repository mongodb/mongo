/**
 * Tests that shard filtering works as expected when the DISTINCT_SCAN optimization is applied to a
 * $group with the $top and $bottom accumulators.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   do_not_wrap_aggregations_in_facets,
 *   not_allowed_with_signed_security_token,
 *   expects_explicit_underscore_id_index,
 *   # Index filter commands do not support causal consistency.
 *   does_not_support_causal_consistency,
 *   # TODO SERVER-95934: Remove tsan_incompatible.
 *   tsan_incompatible,
 * ]
 */

import {
    prepareShardedCollectionWithOrphans
} from "jstests/libs/query/group_to_distinct_scan_utils.js";
import {
    runGroupWithTopBottomToDistinctScanTests
} from "jstests/libs/query/group_with_top_bottom_to_distinct_scan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2});
const db = prepareShardedCollectionWithOrphans(st);

runGroupWithTopBottomToDistinctScanTests(db, true);

st.stop();
