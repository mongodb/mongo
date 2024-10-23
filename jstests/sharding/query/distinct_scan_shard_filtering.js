/**
 * Tests that shard filtering works as expected for DISTINCT_SCAN queries where shard filtering
 * doesn't require fetching.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   do_not_wrap_aggregations_in_facets,
 *   not_allowed_with_signed_security_token,
 *   # TODO SERVER-95934: Remove tsan_incompatible and aubsan_incompatible.
 *   tsan_incompatible,
 *   incompatible_aubsan
 * ]
 */

import {getWinningPlan, planHasStage} from "jstests/libs/query/analyze_plan.js";
import {
    assertPipelineResultsAndExplain,
    assertPlanDoesNotUseDistinctScan,
    assertPlanUsesCollScan,
    assertPlanUsesDistinctScan,
    assertPlanUsesIndexScan,
    coll,
    prepareCollection
} from "jstests/libs/query/group_to_distinct_scan_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2});

const db = st.getDB("test");
const primaryShard = st.shard0.shardName;
const otherShard = st.shard1.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard}));

prepareCollection(db);

// Shard the collection and move all docs where 'a' >= 2 to the non-primary shard.
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {a: 2}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {a: 2}, to: otherShard}));

// Insert orphans to both shards.
const primaryShardOrphanDocs = [
    {a: 2.1, b: "orphan", c: "orphan"},
    {a: 2.2, b: "orphan", c: "orphan"},
    {a: 2.3, b: "orphan", c: "orphan"},
    {a: 999.1, b: "orphan", c: "orphan"},
];
const otherShardOrphanDocs = [
    {a: 0.1, b: "orphan", c: "orphan"},
    {a: 1.1, b: "orphan", c: "orphan"},
    {a: 1.2, b: "orphan", c: "orphan"},
    {a: 1.3, b: "orphan", c: "orphan"},
];
assert.commandWorked(st.shard0.getCollection(coll.getFullName()).insert(primaryShardOrphanDocs));
assert.commandWorked(st.shard1.getCollection(coll.getFullName()).insert(otherShardOrphanDocs));

function assertDistinctResultsAndExplain({field, query, expectedOutput, validateExplain}) {
    const result = coll.distinct(field, query);
    assert.eq(result.sort(), expectedOutput.sort());

    const explain = coll.explain("queryPlanner").distinct("a");
    validateExplain(explain);
}

function assertCoveredDistinctScanPlan(explain) {
    const winningPlan = getWinningPlan(explain.queryPlanner);
    assert(planHasStage(db, winningPlan, "PROJECTION_COVERED"));
    assert(planHasStage(db, winningPlan, "DISTINCT_SCAN"));
}

// Test the distinct command.
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

// Test $group with $top and $bottom. These tests are adapted from
// 'group_with_top_bottom_to_distinct_scan.js' but (for now) only include cases where shard
// filtering doesn't require fetching.
//
// TODO SERVER-92459: Refactor all 'group_with_*_distinct_scan.js' tests such that we can reuse
// these tests programmatically. This should be easy once the SHARDING_FILTER + FETCH +
// DISTINCT_SCAN combination is supported as the shard key doesn't have to be compatible with the
// $group _id any more.

// Verifies that a $group pipeline can use DISTINCT_SCAN when the sort within a $top accumulator is
// available from an index.
assertPipelineResultsAndExplain({
    pipeline: [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}],
    expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group pipeline can use DISTINCT_SCAN when the sort within a $bottom accumulator
// is available from an index.
assertPipelineResultsAndExplain({
    pipeline: [{$group: {_id: "$a", accum: {$bottom: {output: "$b", sortBy: {a: 1, b: 1}}}}}],
    expectedOutput: [{_id: null, accum: 1}, {_id: 1, accum: 3}, {_id: 2, accum: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group pipeline can use DISTINCT_SCAN even when there is a $top accumulator that
// includes an expression.
assertPipelineResultsAndExplain({
    pipeline: [
        {$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: {$add: ["$b", "$c"]}}}}}
    ],
    expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 2}, {_id: 2, accum: 4}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group pipeline can use DISTINCT_SCAN even when there is a $bottom accumulator
// that includes an expression.
assertPipelineResultsAndExplain({
    pipeline: [
        {
            $group:
                {_id: "$a", accum: {$bottom: {sortBy: {a: 1, b: 1}, output: {$add: ["$b", "$c"]}}}}
        },
        /* There are 2 documents with {a: null}, one of which is missing that key entirely.
         * Because those compare equal, we don't know whether the $last one will be the one
         * matching {c: 1} or {c: 1.5}. To make the test deterministic, we add 0.6 to
         * whatever result we get and round it to the nearest integer. */
        {$project: {_id: "$_id", accum: {$round: [{$add: ["$accum", 0.6]}, 0]}}}
    ],
    expectedOutput: [{_id: null, accum: 3.0}, {_id: 1, accum: 6}, {_id: 2, accum: 5}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group can use a DISTINCT_SCAN even when the requested sort is the reverse of the
// index's sort.
assertPipelineResultsAndExplain({
    pipeline: [{$group: {_id: "$a", accum: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}}}],
    expectedOutput: [{_id: null, accum: 1}, {_id: 1, accum: 3}, {_id: 2, accum: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}}}],
    expectedOutput: [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group pipeline _does not_ use DISTINCT_SCAN when there are both $top and $bottom
// accumulators.
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

// Verifies that a $group pipeline with multiple $top accumulators uses DISTINCT_SCAN.
assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            f1: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}},
            f2: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}
        }
    }],
    expectedOutput: [{_id: null, f1: 1, f2: 1}, {_id: 1, f1: 3, f2: 3}, {_id: 2, f1: 2, f2: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [{
        $group: {
            _id: "$a",
            fb: {$top: {sortBy: {a: -1, b: -1, c: -1}, output: "$b"}},
            fc: {$top: {sortBy: {a: -1, b: -1, c: -1}, output: "$c"}}
        }
    }],
    expectedOutput: [{_id: null, fb: 1, fc: 1.5}, {_id: 1, fb: 3, fc: 2}, {_id: 2, fb: 2, fc: 2}],
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group pipeline with multiple $bottom accumulators uses DISTINCT_SCAN.
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
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
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
    validateExplain: (explain) => assertPlanUsesDistinctScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group with mixed accumulators out of $top/$bottom/$first/$last _does not_ use
// DISTINCT_SCAN.
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
        {$group: {_id: "$a", t: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}, f: {$first: "$b"}}}
    ],
    expectedOutput: [{_id: null, t: 1, f: 1}, {_id: 1, t: 3, f: 3}, {_id: 2, t: 2, f: 2}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1}},
        {$group: {_id: "$a", t: {$top: {sortBy: {a: -1, b: -1}, output: "$b"}}, l: {$last: "$b"}}}
    ],
    expectedOutput: [{_id: null, t: 1, l: null}, {_id: 1, t: 3, l: 1}, {_id: 2, t: 2, l: 2}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1}},
        {
            $group:
                {_id: "$a", b: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}, l: {$last: "$b"}}
        }
    ],
    expectedOutput: [{_id: null, b: null, l: null}, {_id: 1, b: 1, l: 1}, {_id: 2, b: 2, l: 2}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [
        {$sort: {a: -1, b: -1}},
        {
            $group:
                {_id: "$a", b: {$bottom: {sortBy: {a: -1, b: -1}, output: "$b"}}, f: {$first: "$b"}}
        }
    ],
    expectedOutput: [{_id: null, b: null, f: 1}, {_id: 1, b: 1, f: 3}, {_id: 2, b: 2, f: 2}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

assertPipelineResultsAndExplain({
    pipeline: [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", f: {$first: "$b"}, l: {$last: "$b"}}}],
    expectedOutput: [{_id: null, f: 1, l: null}, {_id: 1, f: 3, l: 1}, {_id: 2, f: 2, l: 2}],
    validateExplain: (explain) => assertPlanUsesIndexScan(explain, {a: 1, b: 1, c: 1}),
});

// Verifies that a $group pipeline _does not_ use DISTINCT_SCAN when documents are not sorted by the
// field used for grouping.
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

st.stop();
