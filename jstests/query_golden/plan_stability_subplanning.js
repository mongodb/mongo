/**
 * Plan stability test for subplanning of rooted $or queries. Tests that subplanning:
 *
 *   1. Avoids bias toward the first branch — it selects the optimal index per branch rather
 *      than being constrained by whole-query multi-planning filling its result batch on the
 *      first branch (see SERVER-36393, SERVER-46904).
 *
 *   2. Enumerates more candidate plans than whole-query planning for large $or queries — it
 *      explores the full per-branch plan space (10 indexes × 6 branches = 60 sub-plans)
 *      whereas whole-query planning would need to consider 10^6 = 1 M plans and gives up
 *      before finding the optimal one.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_transactions,
 *   assumes_read_concern_local,
 *   incompatible_aubsan,
 *   tsan_incompatible,
 * ]
 */

import {runPlanStabilityPipelines} from "jstests/query_golden/libs/utils.js";

// Ensure subplanning is enabled for the duration of this test.
assert(
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryPlanOrChildrenIndependently: true}))
        .internalQueryPlanOrChildrenIndependently,
    "Expected internalQueryPlanOrChildrenIndependently to be true, but it was not.",
);

// ---------------------------------------------------------------------------
// Scenario 1: Subplanning avoids bias toward the first branch.
//
// 500 documents where field 'a' is sequential (0..499) and 'b = N - a'.
// Each $or branch therefore has a clearly superior dedicated index.
// Without subplanning, whole-query multi-planning exhausts its result
// budget on the first branch and never properly evaluates the 'b' index
// for the second branch.
// ---------------------------------------------------------------------------
{
    const collName = jsTestName() + "_bias";
    const coll = db[collName];
    coll.drop();

    const N = 500;
    const docs = [];
    for (let i = 0; i < N; i++) {
        docs.push({a: i, b: N - i});
    }
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));

    const pipelines = [
        // First branch is best served by the 'a' index (a ≤ 102 → ~103 docs).
        // Second branch is best served by the 'b' index (b ≤ 102 → ~103 docs).
        [
            {
                $match: {
                    $or: [
                        {a: {$lte: 102}, b: {$gte: 0}},
                        {a: {$gte: 0}, b: {$lte: 102}},
                    ],
                },
            },
        ],
    ];

    runPlanStabilityPipelines(db, collName, pipelines);
    // Temporarily disable subplanning and show that the winning plan misses the ideal index "b" on the second branch.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: false}));
    runPlanStabilityPipelines(db, collName, pipelines);
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: true}));
    coll.drop();
}

// ---------------------------------------------------------------------------
// Scenario 2: Subplanning enumerates more plans than whole-query planning.
//
// 6 documents where fields a–i are always 0 and only j varies (0..5).
// The j index is therefore the only selective one.  With 10 indexes and
// 6 OR branches, whole-query planning would need to evaluate 10^6 = 1 M
// combinations and gives up without finding the optimal plan.
// Subplanning needs only 10 × 6 = 60 evaluations and finds j as the
// optimal index for every branch.
// ---------------------------------------------------------------------------
{
    const collName = jsTestName() + "_enumeration";
    const coll = db[collName];
    coll.drop();

    const N = 6;
    const docs = [];
    for (let i = 0; i < N; i++) {
        docs.push({a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: i});
    }
    assert.commandWorked(coll.insertMany(docs));
    for (const field of ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"]) {
        assert.commandWorked(coll.createIndex({[field]: 1}));
    }

    const pipelines = [
        [
            {
                $match: {
                    $or: [
                        {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 0},
                        {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 1},
                        {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 2},
                        {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 3},
                        {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 4},
                        {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, j: 5},
                    ],
                },
            },
        ],
    ];

    runPlanStabilityPipelines(db, collName, pipelines);
    // Temporarily disable subplanning and show that the winning plan misses the ideal index "b" on the second branch.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: false}));
    runPlanStabilityPipelines(db, collName, pipelines);
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: true}));
    coll.drop();
}
