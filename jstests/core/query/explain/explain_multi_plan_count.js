// Tests that in the case of multiple plans, the plan node above the MultiPlanStage is printed
// correctly both as part of the winning, and rejected plans.
//
// This test is not prepared to handle explain output for sharded collections or when executed
// against a mongos.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   assumes_unsharded_collection,
//   assumes_no_implicit_index_creation,
// ]

import {
    assertExplainCount,
    getRejectedPlan,
    getRejectedPlans,
    getWinningPlan,
    isIndexOnly,
    isIxscan,
} from "jstests/libs/analyze_plan.js";

const coll = db.explain_multi_plan_count;
coll.drop();

// Create two indexes to ensure that the best plan will be picked by the multi-planner. Create
// descending index to avoid index deduplication.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: 1}));
assert.commandWorked(coll.insert([{a: 0}, {a: 1}, {a: 2}, {a: 3}]));

const explain =
    coll.explain("allPlansExecution").find({a: {$in: [1, 3]}, b: {$in: [1, 3]}}).count();

// Check that all plans, both winning and rejected have a structure that excludes the MultiPlanNode
// and continues with the correct plan child below COUNT.

assertExplainCount({explainResults: explain, expectedCount: 0});
assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

const rejectedPlans = getRejectedPlans(explain);
for (let curRejectedPlan of rejectedPlans) {
    const rejectedPlan = getRejectedPlan(curRejectedPlan);
    assert.eq(rejectedPlan.stage, "COUNT");
    assert(isIxscan(db, rejectedPlan));
}

assert(coll.drop());