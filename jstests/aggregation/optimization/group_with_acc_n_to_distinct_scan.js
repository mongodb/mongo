/**
 * $group stages with only $firstN/$lastN/$topN/$bottomN accumulators where N == 1 can be converted
 * into corresponding $first/$last/$top/$bottom accumulators. The goal of this optimization is to
 * hopefully convert the group stage to a DISTINCT_SCAN (if a proper index were to exist).
 *
 * @tags: [
 *   # The sharding and $facet passthrough suites modifiy aggregation pipelines in a way that
 *   # prevents the DISTINCT_SCAN optimization from being applied, which breaks the test.
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   requires_fcv_80,
 *   requires_pipeline_optimization,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {prepareCollection} from "jstests/libs/query/group_to_distinct_scan_utils.js";
import {
    runGroupWithAccNToDistinctScanTests
} from "jstests/libs/query/group_with_acc_n_to_distinct_scan.js";

prepareCollection(db);
runGroupWithAccNToDistinctScanTests(db);
