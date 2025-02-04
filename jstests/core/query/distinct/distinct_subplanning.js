/**
 * Ensures that distinct queries properly utilize subplanning when appropriate.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # Explain does not support stepdowns.
 *   does_not_support_stepdowns,
 *   # Explain cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const coll = db[jsTestName()];
assert(coll.drop());
assert.commandWorked(coll.createIndex({x: 1, y: 1}));
assert.commandWorked(coll.createIndex({x: -1, y: 1}));
assert.commandWorked(coll.createIndex({y: 1, x: 1}));
assert.commandWorked(coll.createIndex({x: 1, z: 1, y: 1}));
assert.commandWorked(coll.insertMany([
    {x: 3, y: 5, z: 7},
    {x: 5, y: 6, z: 5},
    {x: 5, y: 5, z: 4},
    {x: 6, y: 5, z: 3},
    {x: 7, y: 5, z: 8},
    {x: 8, y: 7, z: 3},
    {x: 8, y: 8, z: 3},
]));

function confirmSingleDistinctScan(filter, expected) {
    assertArrayEq({actual: coll.distinct("x", filter), expected: expected});
    const explain = coll.explain().distinct("x", filter);
    const winningPlan = getWinningPlanFromExplain(explain);
    // Confirm the winning plan uses a DISTINCT_SCAN.
    assert(getPlanStage(winningPlan, "DISTINCT_SCAN"), explain);

    // Confirm the winning plan does not use an OR or SUBPLAN stage.
    assert(!getPlanStage(winningPlan, "OR"), explain);
    assert(!getPlanStage(winningPlan, "SUBPLAN"), explain);
}

function confirmSubplanningBehavior(filter, expected) {
    assertArrayEq({actual: coll.distinct("x", filter), expected: expected});
    const explain = coll.explain().distinct("x", filter);
    const winningPlan = getWinningPlanFromExplain(explain);
    // Confirm the winning plan does not use a DISTINCT_SCAN.
    assert(!getPlanStage(winningPlan, "DISTINCT_SCAN"), explain);
    // Confirm the winning plan uses an OR stage.
    assert(getPlanStage(winningPlan, "OR"), explain);
    if (!checkSbeFullyEnabled(db)) {
        // If SBE is not fully enabled, confirm we subplan the distinct query.
        assert(getPlanStage(winningPlan, "SUBPLAN"), explain);
    }
}

confirmSingleDistinctScan({$or: [{x: {$lt: 4}}, {x: {$gt: 6}}]}, [3, 7, 8]);
confirmSingleDistinctScan({$or: [{x: {$eq: 2}}, {x: {$eq: 4}}, {x: {$eq: 6}}]}, [6]);

confirmSubplanningBehavior({$or: [{x: {$gt: 3}}, {y: {$eq: 5}}]}, [3, 5, 6, 7, 8]);
confirmSubplanningBehavior({$or: [{x: {$eq: 5}, z: {$ne: 4}}, {y: {$lt: 7}}]}, [3, 5, 6, 7]);
