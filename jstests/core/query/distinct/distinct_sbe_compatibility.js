/**
 * Ensures that distinct-like pipelines that aren't suitable for a DISTINCT_SCAN conversion utilize
 * SBE (when appropriate).
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # Explain does not support stepdowns.
 *   does_not_support_stepdowns,
 *   # Explain cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   featureFlagShardFilteringDistinctScan
 * ]
 */
import {
    assertEngine,
    getPlanStage,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (!checkSbeRestrictedOrFullyEnabled(db)) {
    quit();
}

const coll = db[jsTestName()];
assert(coll.drop());
assert.commandWorked(coll.insertMany([
    {_id: 1, a: 4, b: 2, c: 3, d: 4},
    {_id: 2, a: 4, b: 3, c: 6, d: 5},
    {_id: 3, a: 5, b: 4, c: 7, d: 5}
]));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));

const distinctPipelines = [
    [{$group: {_id: "$a"}}],
    [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}],
    [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}],
    [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}],
    [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}],
    [{$match: {a: 1}}, {$sort: {b: 1}}, {$group: {_id: "$b"}}],
    [{$match: {a: 1}}, {$sort: {a: 1, b: 1}}, {$group: {_id: "$b"}}],
    [{$match: {a: 1, b: 1}}, {$sort: {b: 1, c: 1}}, {$group: {_id: "$b"}}],
    // Ensure rooted $or branches can use DISTINCT_SCAN if the branches can combine into one scan.
    [{$match: {$or: [{a: {$lt: 0}}, {a: {$gt: 10}}]}}, {$group: {_id: "$a"}}]
];

for (const pipeline of distinctPipelines) {
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    const distinctScanStage = getPlanStage(getWinningPlanFromExplain(explain), "DISTINCT_SCAN");
    // Assert that a DISTINCT_SCAN stage exists.
    assert(distinctScanStage, explain);
}

const nonDistinctPipelines = [
    [{$group: {_id: "$b"}}],
    [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: -1}, output: "$c"}}}}],
    [
        {$sort: {a: 1, b: 1}},
        {$group: {_id: "$a", accum: {$top: {sortBy: {b: 1, a: 1}, output: "$c"}}}}
    ],
    [
        {$match: {d: {$gt: 3}}},
        {$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}
    ],
    // Ensure subplanning $or branches does not use DISTINCT_SCAN.
    [{$match: {$or: [{a: {$gt: 0}, b: {$lt: 10}}, {a: {$lt: 10}}]}}, {$group: {_id: "$a"}}]
];

nonDistinctPipelines.forEach(pipeline => assertEngine(pipeline, "sbe", coll));
