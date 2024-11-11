/**
 * Tests that shard filtering works as expected for distinct queries.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   do_not_wrap_aggregations_in_facets,
 *   not_allowed_with_signed_security_token,
 *   # TODO SERVER-95934: Remove tsan_incompatible.
 *   tsan_incompatible,
 * ]
 */

import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";
import {
    coll,
    prepareShardedCollectionWithOrphans
} from "jstests/libs/query/group_to_distinct_scan_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2});
const db = prepareShardedCollectionWithOrphans(st);

function assertDistinctResultsAndExplain({field, query, expectedOutput, validateExplain}) {
    const result = coll.distinct(field, query);
    assert.eq(result.sort(), expectedOutput.sort());

    const explain = coll.explain("queryPlanner").distinct("a");
    validateExplain(explain);
}

function assertCoveredDistinctScanPlan(explain) {
    const winningPlan = getWinningPlanFromExplain(explain.queryPlanner);
    assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
    assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));
}

assertDistinctResultsAndExplain({
    field: "a",
    expectedOutput: [1, 2, null],
    validateExplain: assertCoveredDistinctScanPlan,
});

assertDistinctResultsAndExplain({
    field: "a",
    query: {b: 1},
    expectedOutput: [1, null],
    validateExplain: assertCoveredDistinctScanPlan,
});

assertDistinctResultsAndExplain({
    field: "a",
    query: {a: {$gt: 0}, b: 1},
    expectedOutput: [1],
    validateExplain: assertCoveredDistinctScanPlan,
});

assertDistinctResultsAndExplain({
    field: "a",
    query: {b: 2, c: 2},
    expectedOutput: [1, 2],
    validateExplain: assertCoveredDistinctScanPlan,
});

st.stop();
