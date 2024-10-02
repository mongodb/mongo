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

import {
    assertPipelineResultsAndExplain,
    assertPlanDoesNotUseDistinctScan,
    assertPlanUsesDistinctScan,
    assertPlanUsesIndexScan,
    prepareCollection,
} from "jstests/libs/query/group_to_distinct_scan_utils.js";

prepareCollection();

//
// Verifies that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index and there is a $firstN accumulator (where N == 1).
//
assertPipelineResultsAndExplain({
    pipeline: [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$firstN: {n: 1, input: "$b"}}}}],
    expectedOutput: [{_id: null, accum: [null]}, {_id: 1, accum: [1]}, {_id: 2, accum: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index and there is a $lastN accumulator (where N == 1).
//
assertPipelineResultsAndExplain({
    pipeline:
        [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$lastN: {n: 1, input: "$b"}}}}],
    expectedOutput: [{_id: null, accum: [null]}, {_id: 1, accum: [1]}, {_id: 2, accum: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group pipeline can use DISTINCT_SCAN when the sort within a $topN
// accumulator (where N == 1) is available from an index.
//
assertPipelineResultsAndExplain({
    pipeline: [{$group: {_id: "$a", accum: {$topN: {n: 1, output: "$b", sortBy: {a: 1, b: 1}}}}}],
    expectedOutput: [{_id: null, accum: [null]}, {_id: 1, accum: [1]}, {_id: 2, accum: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group pipeline can use DISTINCT_SCAN when the sort within a $bottomN accumulator
// (where N == 1) is available from an index.
//
assertPipelineResultsAndExplain({
    pipeline:
        [{$group: {_id: "$a", accum: {$bottomN: {n: 1, output: "$b", sortBy: {a: 1, b: 1}}}}}],
    expectedOutput: [{_id: null, accum: [1]}, {_id: 1, accum: [3]}, {_id: 2, accum: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index and there are multiple $firstN accumulators (where N == 1).
//
assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: 1, b: 1, c: 1}},
        {
            $group:
                {_id: "$a", f1: {$firstN: {n: 1, input: "$b"}}, f2: {$firstN: {n: 1, input: "$c"}}}
        }
    ],
    expectedOutput: [
        {_id: null, f1: [null], f2: [null]},
        {_id: 1, f1: [1], f2: [1]},
        {_id: 2, f1: [2], f2: [2]}
    ],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index and there are multiple $lastN accumulators (where N == 1).
//
assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1, c: -1}},
        {$group: {_id: "$a", f1: {$lastN: {n: 1, input: "$b"}}, f2: {$lastN: {n: 1, input: "$c"}}}}
    ],
    expectedOutput: [
        {_id: null, f1: [null], f2: [null]},
        {_id: 1, f1: [1], f2: [1]},
        {_id: 2, f1: [2], f2: [2]}
    ],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group pipeline with multiple $topN accumulators (where N == 1) uses
// DISTINCT_SCAN.
//
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            t1: {$topN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
            t2: {$topN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}}
        }
    }],
    expectedOutput:
        [{_id: null, t1: [1], t2: [1]}, {_id: 1, t1: [3], t2: [3]}, {_id: 2, t1: [2], t2: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group pipeline with multiple $bottomN accumulators (where N == 1) uses
// DISTINCT_SCAN.
//
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            b1: {$bottomN: {n: 1, sortBy: {a: 1, b: 1}, output: "$b"}},
            b2: {$bottomN: {n: 1, sortBy: {a: 1, b: 1}, output: "$b"}}
        }
    }],
    expectedOutput:
        [{_id: null, b1: [1], b2: [1]}, {_id: 1, b1: [3], b2: [3]}, {_id: 2, b1: [2], b2: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index and there is both a $first and a $firstN accumulator (where N == 1).
//
assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: 1, b: 1}},
        {$group: {_id: "$a", f1: {$first: "$b"}, f2: {$firstN: {n: 1, input: "$b"}}}}
    ],
    expectedOutput:
        [{_id: null, f1: null, f2: [null]}, {_id: 1, f1: 1, f2: [1]}, {_id: 2, f1: 2, f2: [2]}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group pipeline can use DISTINCT_SCAN when the sort is available from an index
// and there is both a $top and a $topN accumulator (where N == 1).
//
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            t1: {$topN: {n: 1, output: "$b", sortBy: {a: 1, b: 1}}},
            t2: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}
        }
    }],
    expectedOutput:
        [{_id: null, t1: [null], t2: null}, {_id: 1, t1: [1], t2: 1}, {_id: 2, t1: [2], t2: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group with mixed accumulators out of $topN/$bottomN/$firstN/$lastN (where N ==
// 1) _does not_ use DISTINCT_SCAN.
//
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            f: {$topN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
            l: {$bottomN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}}
        }
    }],
    expectedOutput:
        [{_id: null, f: [1], l: [null]}, {_id: 1, f: [3], l: [1]}, {_id: 2, f: [2], l: [2]}],
    validateExplain: assertPlanDoesNotUseDistinctScan
});

assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1}},
        {
            $group: {
                _id: "$a",
                t: {$topN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
                f: {$firstN: {n: 1, input: "$b"}}
            }
        }
    ],
    expectedOutput:
        [{_id: null, t: [1], f: [1]}, {_id: 1, t: [3], f: [3]}, {_id: 2, t: [2], f: [2]}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1}},
        {
            $group: {
                _id: "$a",
                b: {$bottomN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
                f: {$firstN: {n: 1, input: "$b"}}
            }
        }
    ],
    expectedOutput:
        [{_id: null, b: [null], f: [1]}, {_id: 1, b: [1], f: [3]}, {_id: 2, b: [2], f: [2]}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1}},
        {$group: {_id: "$a", f: {$firstN: {n: 1, input: "$b"}}, l: {$lastN: {n: 1, input: "$b"}}}}
    ],
    expectedOutput:
        [{_id: null, f: [1], l: [null]}, {_id: 1, f: [3], l: [1]}, {_id: 2, f: [2], l: [2]}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

//
// Verifies that a $group, with the same accumulators but where at least one accumulator has N != 1,
// _does not_ use DISTINCT_SCAN.
//
assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1, c: -1}},
        {$group: {_id: "$a", f1: {$lastN: {n: 1, input: "$b"}}, f2: {$lastN: {n: 2, input: "$c"}}}}
    ],
    expectedOutput: [
        {_id: null, f1: [null], f2: [null, null]},
        {_id: 1, f1: [1], f2: [2, 1]},
        {_id: 2, f1: [2], f2: [2]}
    ],
    validateExplain: assertPlanDoesNotUseDistinctScan
});

assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            t1: {$topN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
            t2: {$topN: {n: 2, sortBy: {a: -1, b: -1}, output: "$b"}}
        }
    }],
    expectedOutput: [
        {_id: null, t1: [1], t2: [1, 1]},
        {_id: 1, t1: [3], t2: [3, 2]},
        {_id: 2, t1: [2], t2: [2]}
    ],
    validateExplain: assertPlanDoesNotUseDistinctScan,
});

//
// Verifies that a $group, with only $topN's where N == 1 but with different sort keys,
// _does not_ use DISTINCT_SCAN.
//
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            t1: {$topN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
            t2: {$topN: {n: 1, sortBy: {a: 1, b: 1}, output: "$b"}}
        }
    }],
    expectedOutput:
        [{_id: null, t1: [1], t2: [null]}, {_id: 1, t1: [3], t2: [1]}, {_id: 2, t1: [2], t2: [2]}],
    validateExplain: assertPlanDoesNotUseDistinctScan,
});

//
// Verifies that a $group, with only $bottomN's where N == 1 but with different sort keys,
// _does not_ use DISTINCT_SCAN.
//
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            b1: {$bottomN: {n: 1, sortBy: {a: -1, b: -1}, output: "$b"}},
            b2: {$bottomN: {n: 1, sortBy: {a: 1, b: 1}, output: "$b"}}
        }
    }],
    expectedOutput:
        [{_id: null, b1: [null], b2: [1]}, {_id: 1, b1: [1], b2: [3]}, {_id: 2, b1: [2], b2: [2]}],
    validateExplain: assertPlanDoesNotUseDistinctScan,
});
