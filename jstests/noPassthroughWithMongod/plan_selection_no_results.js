/**
 * Tests behavior for plan selection when two candidate plans both return no results.
 *
 * The plan which is able to most cheaply determine that there are no results should be selected as
 * the winner.
 */
import {getPlanStage} from "jstests/libs/analyze_plan.js";

const coll = db.plan_selection_no_results;
coll.drop();

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

const predicate = {
    x: "x val",
    y: "y val",
};

for (let i = -100; i < 0; ++i) {
    // None of these documents will match the predicate.
    assert.commandWorked(coll.insert({x: "x val", y: "NOT y val"}));
}

for (let i = 0; i < 50; ++i) {
    // None of these match the predicate either.
    assert.commandWorked(coll.insert({x: "NOT x val", y: "y val"}));
}

// Run the query, and see which plan wins. Neither plan will return any results though the plan
// using the 'y' index will only have to examine the docs with z greater 0. This should be the
// winning plan.

const explain = coll.find(predicate).explain();
const ixScan = getPlanStage(explain, "IXSCAN");
assert.eq(ixScan.keyPattern, {y: 1}, explain);

// Check that there's two rejected plans (one IX intersect plan and one plan which scans the
// {x: 1} index).
assert.eq(explain.queryPlanner.rejectedPlans.length, 2, explain);
