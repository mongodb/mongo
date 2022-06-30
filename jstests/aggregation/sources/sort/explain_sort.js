// Tests the behavior of explain() when used with the aggregation pipeline and sort. This test is
// designed to reproduce SERVER-33084.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   # Asserts on the number of documents examined in an explain plan.
//   assumes_no_implicit_index_creation
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

const coll = db.explain_sort;
coll.drop();

const kNumDocs = 10;

// Return whether or not explain() was successful and contained the appropriate fields given the
// requested verbosity.
function checkResults(results, verbosity, expectedNumResults = kNumDocs) {
    let cursorSubdocs = getAggPlanStages(results, "$cursor");
    let nReturned = 0;
    let nExamined = 0;
    for (let stageResult of cursorSubdocs) {
        const result = stageResult.$cursor;
        if (verbosity === "queryPlanner") {
            assert(!result.hasOwnProperty("executionStats"), results);
        } else if (cursorSubdocs.length === 1) {
            // If there was a single shard, then we can assert that 'nReturned' and
            // 'totalDocsExamined' are as expected. If there are multiple shards, these assertions
            // might not hold, since each shard enforces the limit on its own and then the merging
            // node enforces the limit again to obtain the final result set.
            assert.eq(result.executionStats.nReturned, expectedNumResults, results);
            assert.eq(result.executionStats.totalDocsExamined, expectedNumResults, results);
        }
    }

    // If there was no $cursor stage, then assert that the pipeline was optimized away.
    if (cursorSubdocs.length === 0) {
        assert(isQueryPlan(results), results);
    }
}

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Execute several aggregations with a sort stage combined with various single document
// transformation stages.
for (let verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
    let pipeline = [{$project: {a: 1}}, {$sort: {a: 1}}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity);

    pipeline = [{$project: {a: 0}}, {$sort: {a: 1}}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity);

    pipeline = [{$addFields: {b: 1}}, {$sort: {a: 1}}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity);

    pipeline = [{$sort: {a: 1}}, {$project: {_id: 1}}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity);

    const res = db.adminCommand({getParameter: 1, "failpoint.disablePipelineOptimization": 1});
    assert.commandWorked(res);
    const optimizeDisabled = res["failpoint.disablePipelineOptimization"].mode;

    pipeline = [{$project: {a: 1}}, {$limit: 5}, {$sort: {a: 1}}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity, optimizeDisabled ? 10 : 5);

    pipeline = [{$project: {_id: 1}}, {$limit: 5}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity, optimizeDisabled ? 10 : 5);
}
})();
