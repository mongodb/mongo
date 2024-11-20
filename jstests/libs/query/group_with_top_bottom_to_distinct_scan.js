/**
 * $group stages with only $top or only $bottom accumulators can sometimes be converted into a
 * DISTINCT_SCAN (see SERVER-84347). This optimization potentially applies to a $group when it
 * begins the pipeline or when it is preceded only by $match. In all cases, it must be possible to
 * do a DISTINCT_SCAN that sees each value of the distinct field exactly once among matching
 * documents and also provides any requested sort ('sortBy' field). The test queries below show most
 * $match/$group combinations where that is possible.
 */

import {
    addIndex,
    assertPipelineResultsAndExplain,
    assertPlanDoesNotUseDistinctScan,
    assertPlanUsesCollScan,
    assertPlanUsesDistinctScan,
    assertPlanUsesIndexScan,
    removeIndex,
} from "jstests/libs/query/group_to_distinct_scan_utils.js";

export function runGroupWithTopBottomToDistinctScanTests(database) {
    /**
     * The tests below should only pass once distinct scan for $top and $bottom accumulators is
     * achieved.
     */

    // Verifies that a $group pipeline can use DISTINCT_SCAN when the sort within a $top accumulator
    // is available from an index.
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    // Verifies that a $group pipeline can use DISTINCT_SCAN when the sort within a $bottom
    // accumulator is available from an index.
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$a", accum: {$bottom: {output: "$b", sortBy: {a: 1, b: 1}}}}}],
        expectedOutput: [{_id: null, accum: 1}, {_id: 1, accum: 3}, {_id: 2, accum: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline can use DISTINCT_SCAN when sorting by fields with dotted
    // paths.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group:
                {_id: "$foo.a", accum: {$top: {sortBy: {"foo.a": 1, "foo.b": 1}, output: "$foo.b"}}}
        }],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: 1, accum: 1},
            {_id: 2, accum: 2},
            {_id: 3, accum: null}
        ],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {"foo.a": 1, "foo.b": 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$foo.a",
                accum: {$bottom: {sortBy: {"foo.a": 1, "foo.b": 1}, output: "$foo.b"}}
            }
        }],
        expectedOutput:
            [{_id: null, accum: 1}, {_id: 1, accum: 2}, {_id: 2, accum: 2}, {_id: 3, accum: null}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {"foo.a": 1, "foo.b": 1}),
    });

    //
    // Verifies that we _do not_ attempt to use a DISTINCT_SCAN on a multikey dotted-path field.
    //
    assertPipelineResultsAndExplain({
        pipeline: [
            {$sort: {"mkFoo.a": 1, "mkFoo.b": 1}},
            {$group: {_id: "$mkFoo.a", accum: {$first: "$mkFoo.b"}}}
        ],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: 1, accum: 1},
            {_id: 2, accum: 2},
            {_id: [3, 4], accum: [4, 3]}
        ],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    //
    // Verifies that we use a DISTINCT_SCAN to satisfy a sort on a multikey field if the bounds
    // are [minKey, maxKey].
    //
    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$aa", accum: {$top: {sortBy: {aa: 1, mkB: 1}, output: "$mkB"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: [1, 3]}, {_id: 2, accum: []}],
        validateExplain: (explain) => {
            assertPlanUsesDistinctScan(database, explain, {aa: 1, mkB: 1, c: 1}, true);
        }
    });

    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$aa", accum: {$top: {sortBy: {aa: -1, mkB: -1}, output: "$mkB"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: [1, 3]}, {_id: 2, accum: []}],
        validateExplain: (explain) => {
            assertPlanUsesDistinctScan(database, explain, {aa: 1, mkB: 1, c: 1}, true);
        }
    });

    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$aa", accum: {$bottom: {sortBy: {aa: -1, mkB: -1}, output: "$mkB"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 2}, {_id: 2, accum: []}],
        validateExplain: assertPlanDoesNotUseDistinctScan
    });

    //
    // Verifies that with dotted paths we use a DISTINCT_SCAN to satisfy a sort on a multikey field
    // if the bounds are [minKey, maxKey].
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$foo.a",
                accum: {$top: {sortBy: {"foo.a": 1, "mkFoo.b": 1}, output: "$mkFoo.b"}}
            }
        }],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: 1, accum: 1},
            {_id: 2, accum: 2},
            {_id: 3, accum: [4, 3]}
        ],
        validateExplain: (explain) => {
            assertPlanUsesDistinctScan(database, explain, {"foo.a": 1, "mkFoo.b": 1}, true);
        },
    });

    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$foo.a",
                accum: {$bottom: {sortBy: {"foo.a": 1, "mkFoo.b": -1}, output: "$mkFoo.b"}}
            }
        }],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: 1, accum: 1},
            {_id: 2, accum: 2},
            {_id: 3, accum: [4, 3]}
        ],
        validateExplain: assertPlanUsesCollScan,
    });

    //
    // Verifies that we _do not_ attempt a DISTINCT_SCAN to satisfy a sort on a multikey field if
    // the bounds are not [minKey, maxKey].
    //
    assertPipelineResultsAndExplain({
        pipeline: [
            {$match: {mkB: {$ne: 9999}}},
            {$group: {_id: "$aa", accum: {$top: {sortBy: {aa: 1, mkB: 1}, output: "$mkB"}}}}
        ],
        expectedOutput: [{_id: 1, accum: [1, 3]}, {_id: null, accum: null}, {_id: 2, accum: []}],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$match: {mkB: {$gt: -5}}},
            {$group: {_id: "$aa", accum: {$top: {sortBy: {aa: 1, mkB: 1}, output: "$mkB"}}}}
        ],
        expectedOutput: [{_id: 1, accum: [1, 3]}],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    // Repeats above tests for $last.
    assertPipelineResultsAndExplain({
        pipeline: [
            {$match: {mkB: {$ne: 9999}}},
            {$group: {_id: "$aa", accum: {$bottom: {sortBy: {aa: 1, mkB: 1}, output: "$mkB"}}}}
        ],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 2}, {_id: 2, accum: []}],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$match: {mkB: {$gt: -5}}},
            {$group: {_id: "$aa", accum: {$bottom: {sortBy: {aa: 1, mkB: 1}, output: "$mkB"}}}}
        ],
        expectedOutput: [{_id: 1, accum: 2}],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    //
    // Verifies that with dotted paths we _do not_ attempt a DISTINCT_SCAN to satisfy a sort on a
    // multikey field if the bounds are not [minKey, maxKey].
    //
    assertPipelineResultsAndExplain({
        pipeline: [
            {$match: {"mkFoo.b": {$ne: 20000}}},
            {
                $group: {
                    _id: "$foo.a",
                    accum: {$top: {sortBy: {"foo.a": 1, "mkFoo.b": 1}, output: "$mkFoo.b"}}
                }
            }
        ],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: 1, accum: 1},
            {_id: 2, accum: 2},
            {_id: 3, accum: [4, 3]}
        ],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$match: {"mkFoo.b": {$ne: 20000}}},
            {
                $group: {
                    _id: "$foo.a",
                    accum: {$bottom: {sortBy: {"foo.a": 1, "mkFoo.b": -1}, output: "$mkFoo.b"}}
                }
            }
        ],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: 1, accum: 1},
            {_id: 2, accum: 2},
            {_id: 3, accum: [4, 3]}
        ],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    //
    // Verifies that we can use a DISTINCT_SCAN on a multikey index to group with $top or $bottom
    // with sortBy a dotted-path field, so long as the field we are sorting over is not multikey and
    // comes before any multikey fields in the index key pattern.
    //
    // We drop the {"foo.a": 1, "foo.b": 1} to force this test to use the multikey
    // {"foo.a": 1, "mkFoo.b"} index. The rest of the test doesn't use either of those indexes.
    //
    removeIndex({"foo.a": 1, "foo.b": 1});

    //
    // Verifies that a $group pipeline can use DISTINCT_SCAN even when there is a $top accumulator
    // that accesses a multikey field.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$aa", accum: {$top: {sortBy: {aa: 1, bb: 1}, output: "$mkB"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: [1, 3]}, {_id: 2, accum: []}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {aa: 1, bb: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline can use DISTINCT_SCAN when there is a $bottom accumulator
    // that accesses a multikey field, provided that field is not part of the index.
    //
    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$aa", accum: {$bottom: {sortBy: {aa: 1, bb: 1}, output: "$mkB"}}}}],
        expectedOutput: [{_id: 1, accum: 2}, {_id: 2, accum: []}, {_id: null, accum: null}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {aa: 1, bb: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline can use DISTINCT_SCAN even when there is a $top accumulator
    // that includes an expression.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group:
                {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: {$add: ["$b", "$c"]}}}}
        }],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 2}, {_id: 2, accum: 4}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline can use DISTINCT_SCAN even when there is a $bottom
    // accumulator that includes an expression.
    //
    assertPipelineResultsAndExplain({
        pipeline: [
            {
                $group: {
                    _id: "$a",
                    accum: {$bottom: {sortBy: {a: 1, b: 1}, output: {$add: ["$b", "$c"]}}}
                }
            },
            /* There are 2 documents with {a: null}, one of which is missing that key entirely.
             * Because those compare equal, we don't know whether the $last one will be the one
             * matching {c: 1} or {c: 1.5}. To make the test deterministic, we add 0.6 to
             * whatever result we get and round it to the nearest integer. */
            {$project: {_id: "$_id", accum: {$round: [{$add: ["$accum", 0.6]}, 0]}}}
        ],
        expectedOutput: [{_id: null, accum: 3.0}, {_id: 1, accum: 6}, {_id: 2, accum: 5}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group can use a DISTINCT_SCAN even when the requested sort is the reverse of
    // the index's sort.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$a", accum: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}}}],
        expectedOutput: [{_id: null, accum: 1}, {_id: 1, accum: 3}, {_id: 2, accum: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline _does not_ use DISTINCT_SCAN when there are both $top and
    // $bottom accumulators.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$a",
                f: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}},
                l: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}
            }
        }],
        expectedOutput: [{_id: null, f: 1, l: null}, {_id: 1, f: 3, l: 1}, {_id: 2, f: 2, l: 2}],
        validateExplain: assertPlanDoesNotUseDistinctScan
    });

    //
    // Verifies that a $group pipeline with multiple $top accumulators uses DISTINCT_SCAN.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$a",
                f1: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}},
                f2: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}
            }
        }],
        expectedOutput: [{_id: null, f1: 1, f2: 1}, {_id: 1, f1: 3, f2: 3}, {_id: 2, f1: 2, f2: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$a",
                fb: {$top: {sortBy: {a: -1, b: -1, c: -1}, output: "$b"}},
                fc: {$top: {sortBy: {a: -1, b: -1, c: -1}, output: "$c"}}
            }
        }],
        expectedOutput:
            [{_id: null, fb: 1, fc: 1.5}, {_id: 1, fb: 3, fc: 2}, {_id: 2, fb: 2, fc: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline with multiple $bottom accumulators uses DISTINCT_SCAN.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$a",
                l1: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}},
                l2: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}
            }
        }],
        expectedOutput:
            [{_id: null, l1: null, l2: null}, {_id: 1, l1: 1, l2: 1}, {_id: 2, l1: 2, l2: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$a",
                lb: {$bottom: {sortBy: {a: -1, b: -1, c: -1}, output: "$b"}},
                lc: {$bottom: {sortBy: {a: -1, b: -1, c: -1}, output: "$c"}}
            }
        }],
        expectedOutput:
            [{_id: null, lb: null, lc: null}, {_id: 1, lb: 1, lc: 1}, {_id: 2, lb: 2, lc: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group with mixed accumulators out of $top/$bottom/$first/$last _does not_
    // use DISTINCT_SCAN.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{
            $group: {
                _id: "$a",
                t: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}},
                b: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}
            }
        }],
        expectedOutput: [{_id: null, t: 1, b: null}, {_id: 1, t: 3, b: 1}, {_id: 2, t: 2, b: 2}],
        validateExplain: (explain) => assertPlanUsesCollScan(explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$sort: {a: -1, b: -1}},
            {
                $group: {
                    _id: "$a",
                    t: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}},
                    f: {$first: "$b"}
                }
            }
        ],
        expectedOutput: [{_id: null, t: 1, f: 1}, {_id: 1, t: 3, f: 3}, {_id: 2, t: 2, f: 2}],
        validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$sort: {a: -1, b: -1}},
            {
                $group:
                    {_id: "$a", t: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}, l: {$last: "$b"}}
            }
        ],
        expectedOutput: [{_id: null, t: 1, l: null}, {_id: 1, t: 3, l: 1}, {_id: 2, t: 2, l: 2}],
        validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$sort: {a: -1, b: -1}},
            {
                $group: {
                    _id: "$a",
                    b: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}},
                    l: {$last: "$b"}
                }
            }
        ],
        expectedOutput: [{_id: null, b: null, l: null}, {_id: 1, b: 1, l: 1}, {_id: 2, b: 2, l: 2}],
        validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline: [
            {$sort: {a: -1, b: -1}},
            {
                $group: {
                    _id: "$a",
                    b: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}},
                    f: {$first: "$b"}
                }
            }
        ],
        expectedOutput: [{_id: null, b: null, f: 1}, {_id: 1, b: 1, f: 3}, {_id: 2, b: 2, f: 2}],
        validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline:
            [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", f: {$first: "$b"}, l: {$last: "$b"}}}],
        expectedOutput: [{_id: null, f: 1, l: null}, {_id: 1, f: 3, l: 1}, {_id: 2, f: 2, l: 2}],
        validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline _does not_ use DISTINCT_SCAN when documents are not sorted by
    // the field used for grouping.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$a", accum: {$top: {sortBy: {b: 1}, output: "$b"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$a", accum: {$bottom: {sortBy: {b: -1}, output: "$b"}}}}],
        expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}],
        validateExplain: assertPlanDoesNotUseDistinctScan,
    });

    //
    // Verifies that a $group pipeline with a $top accumulator can use DISTINCT_SCAN, even when the
    // group _id field is a singleton object instead of a fieldPath.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: {v: "$a"}, accum: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}}],
        expectedOutput:
            [{_id: {v: null}, accum: null}, {_id: {v: 1}, accum: 1}, {_id: {v: 2}, accum: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    //
    // Verifies that a $group pipeline with a $bottom accumulator can use DISTINCT_SCAN, even when
    // the group _id field is a singleton object instead of a fieldPath.
    //
    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: {v: "$a"}, accum: {$bottom: {sortBy: {a: 1, b: 1}, output: "$b"}}}}],
        expectedOutput:
            [{_id: {v: null}, accum: 1}, {_id: {v: 1}, accum: 3}, {_id: {v: 2}, accum: 2}],
        validateExplain: (explain) =>
            assertPlanUsesDistinctScan(database, explain, {a: 1, b: 1, c: 1}),
    });

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // We execute all the collation-related tests three times with three different configurations
    // (no index, index without collation, index with collation).
    //
    // Collation tests 1: no index on string field.
    ////////////////////////////////////////////////////////////////////////////////////////////////

    const collationOption = {collation: {locale: "en_US", strength: 2}};

    //
    // Verifies that a $group pipeline uses a collection scan.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: 1}, output: "$d"}}}}],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: "FoO", accum: 2},
            {_id: "bAr", accum: 3},
            {_id: "bar", accum: 4},
            {_id: "foo", accum: 1}
        ],
        validateExplain: assertPlanUsesCollScan,
    });

    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$str", accum: {$bottom: {sortBy: {str: 1, d: 1}, output: "$d"}}}}],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: "FoO", accum: 2},
            {_id: "bAr", accum: 3},
            {_id: "bar", accum: 4},
            {_id: "foo", accum: 1}
        ],
        validateExplain: assertPlanUsesCollScan,
    });

    //
    // Verifies that a collated $group pipeline with a $top accumulator uses a collection scan.
    //
    assertPipelineResultsAndExplain({
        // Uses $toLower on the group-by string key to produce a stable results. This converts _id:
        // null
        // to _id: "".
        pipeline: [
            {$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: 1}, output: "$d"}}}},
            {$addFields: {_id: {$toLower: "$_id"}}}
        ],
        expectedOutput: [{_id: "", accum: null}, {_id: "bar", accum: 3}, {_id: "foo", accum: 1}],
        validateExplain: assertPlanUsesCollScan,
        options: collationOption,
    });

    //
    // Verifies that a collated $group pipeline with a $bottom accumulator uses a collection scan.
    //
    assertPipelineResultsAndExplain({
        // Uses $toLower on the group-by string key to produce a stable results. This converts _id:
        // null
        // to _id: "".
        pipeline: [
            {$group: {_id: "$str", accum: {$bottom: {sortBy: {str: 1, d: 1}, output: "$d"}}}},
            {$addFields: {_id: {$toLower: "$_id"}}}
        ],
        expectedOutput: [{_id: "", accum: null}, {_id: "bar", accum: 4}, {_id: "foo", accum: 2}],
        validateExplain: assertPlanUsesCollScan,
        options: collationOption,
    });

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // Collation tests 2: index on string field with no collation.
    ////////////////////////////////////////////////////////////////////////////////////////////////

    addIndex({str: 1, d: 1});

    //
    // Verifies that a $group pipeline uses a DISTINCT_SCAN.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: 1}, output: "$d"}}}}],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: "FoO", accum: 2},
            {_id: "bAr", accum: 3},
            {_id: "bar", accum: 4},
            {_id: "foo", accum: 1}
        ],
        validateExplain: (explain) => assertPlanUsesDistinctScan(database, explain, {str: 1, d: 1}),
    });

    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$str", accum: {$bottom: {sortBy: {str: 1, d: 1}, output: "$d"}}}}],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: "FoO", accum: 2},
            {_id: "bAr", accum: 3},
            {_id: "bar", accum: 4},
            {_id: "foo", accum: 1}
        ],
        validateExplain: (explain) => assertPlanUsesDistinctScan(database, explain, {str: 1, d: 1}),
    });

    //
    // Verifies that a $group that use a collation and includes a $top accumulators _does not_ scan
    // the index, which is not aware of the collation.
    //
    assertPipelineResultsAndExplain({
        // Uses $toLower on the group-by string key to produce a stable results. This converts _id:
        // null
        // to _id: "".
        pipeline: [
            {$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: 1}, output: "$d"}}}},
            {$addFields: {_id: {$toLower: "$_id"}}}
        ],
        expectedOutput: [{_id: "", accum: null}, {_id: "bar", accum: 3}, {_id: "foo", accum: 1}],
        validateExplain: assertPlanUsesCollScan,
        options: collationOption
    });

    //
    // Verifies that a $group that use a collation and includes a $last accumulators _does not_ scan
    // the index, which is not aware of the collation.
    //
    assertPipelineResultsAndExplain({
        // Uses $toLower on the group-by string key to produce a stable results. This converts _id:
        // null
        // to _id: "".
        pipeline: [
            {$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: -1}, output: "$d"}}}},
            {$addFields: {_id: {$toLower: "$_id"}}}
        ],
        expectedOutput: [{_id: "bar", accum: 4}, {_id: "foo", accum: 2}, {_id: "", accum: null}],
        validateExplain: assertPlanUsesCollScan,
        options: collationOption
    });

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // Collation tests 3: index on string field with case-insensitive collation.
    ////////////////////////////////////////////////////////////////////////////////////////////////

    removeIndex({str: 1, d: 1});
    addIndex({str: 1, d: 1}, collationOption);

    //
    // Verifies that a $group pipeline with no collation _does not_ scan the index, which does have
    // a collation.
    //
    assertPipelineResultsAndExplain({
        pipeline: [{$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: 1}, output: "$d"}}}}],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: "FoO", accum: 2},
            {_id: "bAr", accum: 3},
            {_id: "bar", accum: 4},
            {_id: "foo", accum: 1}
        ],
        validateExplain: assertPlanUsesCollScan
    });

    assertPipelineResultsAndExplain({
        pipeline:
            [{$group: {_id: "$str", accum: {$bottom: {sortBy: {str: 1, d: 1}, output: "$d"}}}}],
        expectedOutput: [
            {_id: null, accum: null},
            {_id: "FoO", accum: 2},
            {_id: "bAr", accum: 3},
            {_id: "bar", accum: 4},
            {_id: "foo", accum: 1}
        ],
        validateExplain: assertPlanUsesCollScan
    });

    //
    // Verifies that a $group pipeline that uses a collation and includes a $top accumulator uses a
    // DISTINCT_SCAN, which uses a matching collation.
    //
    assertPipelineResultsAndExplain({
        // Uses $toLower on the group-by string key to produce a stable results. This converts _id:
        // null
        // to _id: "".
        pipeline: [
            {$group: {_id: "$str", accum: {$top: {sortBy: {str: 1, d: 1}, output: "$d"}}}},
            {$addFields: {_id: {$toLower: "$_id"}}}
        ],
        expectedOutput: [{_id: "", accum: null}, {_id: "bar", accum: 3}, {_id: "foo", accum: 1}],
        validateExplain: (explain) => assertPlanUsesDistinctScan(database, explain, {str: 1, d: 1}),
        options: collationOption
    });

    //
    // Verifies that a $group pipeline that uses a collation and includes a $bottom accumulator uses
    // a DISTINCT_SCAN, which uses a matching collation.
    //
    assertPipelineResultsAndExplain({
        // Uses $toLower on the group-by string key to produce a stable results. This converts _id:
        // null
        // to _id: "".
        pipeline: [
            {$group: {_id: "$str", accum: {$bottom: {sortBy: {str: 1, d: 1}, output: "$d"}}}},
            {$addFields: {_id: {$toLower: "$_id"}}}
        ],
        expectedOutput: [{_id: "foo", accum: 2}, {_id: "", accum: null}, {_id: "bar", accum: 4}],
        validateExplain: (explain) => assertPlanUsesDistinctScan(database, explain, {str: 1, d: 1}),
        options: collationOption
    });

    // These tests do not verify data but verify that the server does not die.
    assert.eq(
        database.nodata.aggregate([{$group: {_id: '$a', o: {$top: {output: 'a', sortBy: {}}}}}])
            .toArray()
            .length,
        0);
    assert.eq(database.nodata
                  .aggregate([{$group: {_id: '$a', o: {$topN: {n: 1, output: 'a', sortBy: {}}}}}])
                  .toArray()
                  .length,
              0);
    assert.eq(
        database.nodata.aggregate([{$group: {_id: '$a', o: {$bottom: {output: 'a', sortBy: {}}}}}])
            .toArray()
            .length,
        0);
    assert.eq(
        database.nodata
            .aggregate([{$group: {_id: '$a', o: {$bottomN: {n: 1, output: 'a', sortBy: {}}}}}])
            .toArray()
            .length,
        0);
}
