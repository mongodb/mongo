// Tests that the winning plan statistics are correctly reported in Explain output at
// "allPlansExecution" verbosity mode.
//
// This test is not prepared to handle explain output for sharded collections.
// @tags: [
//   assumes_unsharded_collection,
//   assumes_against_mongod_not_mongos,
// ]

import {getExecutionStats, getOptimizer} from "jstests/libs/analyze_plan.js";

const coll = db.explain_winning_plan;
coll.drop();

// Create two indexes to ensure that the best plan will be picked by the multi-planner.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: 1}));

// Load the server parameter which instructs the planner to stop when a candidate plan returns that
// many documents.
const res = db.adminCommand({getParameter: 1, internalQueryPlanEvaluationMaxResults: 1});
assert.commandWorked(res);
assert(res.hasOwnProperty("internalQueryPlanEvaluationMaxResults"));
const maxResults = res["internalQueryPlanEvaluationMaxResults"];
assert.gt(maxResults, 0);

// Insert 'maxResults' plus some extra documents into the collection. We expect each candidate plan
// to return 'maxResults' during the trial period, which should be reported in "allPlansExecution"
// section of explain output, and 'numDocs' documents (the entire collection) in the
// "executionStats" section of the winning plan.
const numDocs = maxResults + 20;
assert.commandWorked(coll.insert(Array.from({length: numDocs}, (_, i) => {
    return {_id: i, a: i};
})));

const explain = coll.find({a: {$gte: 0}}).explain("allPlansExecution");

// Make sure the "executionStats" section of the explain output correctly reports the number of
// returned documents for the winning plan after it was executed until EOF.
assert(explain.hasOwnProperty("executionStats"), explain);
assert.eq(explain.executionStats.nReturned, numDocs);

// Make sure the "allPlansExecution" section contains an array with exactly two elements
// representing two candidate plans evaluated by the multi-planner.
assert(explain.executionStats.hasOwnProperty("allPlansExecution"), explain);
// On a sharded cluster, this test assumes that the cluster only has one shard.
const executionStats = getExecutionStats(explain)[0];
assert(Array.isArray(executionStats.allPlansExecution), explain);

switch (getOptimizer(explain)) {
    case "classic":
        assert.eq(executionStats.allPlansExecution.length, 2, explain);
        break;
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
}

// Each candidate plan should have returned exactly 'maxResults' number of documents during the
// trial period.
for (const planStats of executionStats.allPlansExecution) {
    assert(planStats.hasOwnProperty("nReturned"));
    assert.eq(planStats.nReturned, maxResults, explain);
}

// If there was a single plan, allPlansExecution array should be present but empty.
const explainSinglePlan = coll.find().explain("allPlansExecution");
// On a sharded cluster, this test assumes that the cluster only has one shard.
const explainSingleExecutionStats = getExecutionStats(explainSinglePlan)[0];
assert.eq(explainSingleExecutionStats.nReturned, numDocs);
assert(explainSingleExecutionStats.hasOwnProperty("allPlansExecution"), explainSinglePlan);
assert(Array.isArray(explainSingleExecutionStats.allPlansExecution), explainSinglePlan);
assert.eq(explainSingleExecutionStats.allPlansExecution.length, 0, explainSinglePlan);

assert(coll.drop());
