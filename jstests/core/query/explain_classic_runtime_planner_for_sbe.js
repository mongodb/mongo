// Test that explain format for classic multiplanning with SBE features classic explain format for
// queryPlanner and allPlansExecution, but SBE format for executionStats.
// @tags: [
//  assumes_unsharded_collection,
//  featureFlagClassicRuntimePlanningForSbe,
//  featureFlagSbeFull,
// ]

import {
    getExecutionStages,
    getExecutionStats,
    getRejectedPlans,
} from "jstests/libs/analyze_plan.js";

const coll = db.explain_classic_runtime_planner_for_sbe;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

for (let i = 0; i < 500; i++) {
    assert.commandWorked(coll.insert({a: i, b: i * i}));
}

// Execute assertions for two explain plans: one with nResults less than 101, and another with
// nResults greater than 101. This ensures that explain always displays execution stats from SBE,
// even if the entire result set can be computed during classic multi-planning.
const smallExplain = coll.find({b: {$lt: 100}}).sort({a: 1}).explain("allPlansExecution");
const largeExplain = coll.find({b: {$gte: 100}}).sort({a: 1}).explain("allPlansExecution");

assertExplainFormat(smallExplain, 10);
assertExplainFormat(largeExplain, 490);

function assertExplainFormat(explain, expectedNumReturned) {
    const isSharded = explain.queryPlanner.winningPlan.hasOwnProperty("shards");
    const explainVersion = isSharded ? explain.queryPlanner.winningPlan.shards[0].explainVersion
                                     : explain.explainVersion;
    assert.eq(explainVersion, "2", explain);

    // Confirm the number of results is as expected
    const execStatsList = getExecutionStats(explain);
    assert.eq(execStatsList.length, 1, execStatsList);
    const execStats = execStatsList[0];
    assert.eq(execStats.nReturned, expectedNumReturned, explain);

    // rejectedPlans - CLASSIC format:
    for (const plan of getRejectedPlans(explain)) {
        assert(!plan.hasOwnProperty("slotBasedPlan"), explain);
        assert(plan.hasOwnProperty("inputStage"), explain);
        assert(!plan.hasOwnProperty("queryPlan"), explain);
    }

    // executionStats - SBE format:
    const stages = getExecutionStages(explain);
    assert.eq(stages.length, 1, explain);
    const execStage = stages[0]
    assert(execStage.hasOwnProperty("opens"), explain);
    assert(execStage.hasOwnProperty("closes"), explain);
    assert(!execStage.hasOwnProperty("works"), explain);

    // allPlansExecution - CLASSIC format:
    for (const plan of execStats.allPlansExecution) {
        assert.gte(plan.score, 1, explain);
        assert(plan.hasOwnProperty("executionStages"), explain);
        const stages = plan["executionStages"];
        assert(stages.hasOwnProperty("works"), explain);
        assert(!stages.hasOwnProperty("opens"), explain);
        assert(!stages.hasOwnProperty("closes"), explain);
    }
}
