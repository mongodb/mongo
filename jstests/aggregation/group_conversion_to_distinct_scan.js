/**
 * $group stages with no accumulators or with only $first or only $last accumulators can sometimes
 * be converted into a DISTINCT_SCAN (see SERVER-9507 and SERVER-37304). This optimization
 * potentially applies to a $group when it begins the pipeline or when it is preceded only by one or
 * both of $match and $sort (in that order). In all cases, it must be possible to do a DISTINCT_SCAN
 * that sees each value of the distinct field exactly once among matching documents and also
 * provides any requested sort. The test queries below show most $match/$sort/$group combinations
 * where that is possible.
 *
 * @tags: [
 *   # The sharding and $facet passthrough suites modifiy aggregation pipelines in a way that
 *   # prevents the DISTINCT_SCAN optimization from being applied, which breaks the test.
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   # Index filter commands do not support causal consistency.
 *   does_not_support_causal_consistency,
 *   requires_fcv_63,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {
    runGroupConversionToDistinctScanTests
} from "jstests/libs/query/group_conversion_to_distinct_scan.js";
import {prepareCollection} from "jstests/libs/query/group_to_distinct_scan_utils.js";

prepareCollection(db);
runGroupConversionToDistinctScanTests(db);
