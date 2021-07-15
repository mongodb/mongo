// Tests that plan scores are correctly outputted alongside each plan in the 'allPlansExecution'
// field.
//
// This test is not prepared to handle explain output for sharded collections or when executed
// against a mongos.
// @tags: [
//   requires_fcv_51,
//   assumes_unsharded_collection,
//   assumes_against_mongod_not_mongos,
// ]

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.explain_plan_scores;
coll.drop();

// Checks that scores are not outputted alongside the winning and rejected plans in 'queryPlanner'
// and not outputted with the winning plan stats in 'executionStats' at all verbosity levels.
// Scores should only be outputted alongside each plan's trial stats in 'allPlansExecution'.
function checkExplainOutput(explain, verbosity) {
    const winningPlan = getWinningPlan(explain.queryPlanner);
    assert(!winningPlan.hasOwnProperty("score"), explain);

    const rejectedPlans = getRejectedPlans(explain);
    for (let rejectedPlan of rejectedPlans) {
        rejectedPlan = getRejectedPlan(rejectedPlan);
        assert(!rejectedPlan.hasOwnProperty("score"), explain);
    }

    if (verbosity != "queryPlanner") {
        assert(!explain.executionStats.hasOwnProperty("score"), explain);
    }

    if (verbosity == "allPlansExecution") {
        const allPlans = explain.executionStats.allPlansExecution;
        for (let plan of allPlans) {
            assert(plan.hasOwnProperty("score"), explain);
            assert.gt(plan.score, 0, explain);
        }
    }
}

// Create indexes so that there are multiple plans.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

["queryPlanner", "executionStats", "allPlansExecution"].forEach(verbosity => {
    const explain = coll.find({a: {$gte: 0}}).explain(verbosity);
    assert.commandWorked(explain);
    checkExplainOutput(explain, verbosity);
});
}());
