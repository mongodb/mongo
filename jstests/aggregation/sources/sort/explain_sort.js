// Tests the behavior of explain() when used with the aggregation pipeline and sort. This test is
// designed to reproduce SERVER-33084.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

const coll = db.explain_sort;
coll.drop();

const kNumDocs = 10;

// Return whether or not explain() was successful and contained the appropriate fields given the
// requested verbosity.
function checkResults(results, verbosity) {
    let cursorSubdocs = getAggPlanStages(results, "$cursor");
    let nReturned = 0;
    let nExamined = 0;
    assert.gt(cursorSubdocs.length, 0);
    for (let stageResult of cursorSubdocs) {
        const result = stageResult.$cursor;
        if (verbosity === "queryPlanner") {
            assert(!result.hasOwnProperty("executionStats"), tojson(results));
        } else {
            nReturned += result.executionStats.nReturned;
            nExamined += result.executionStats.totalDocsExamined;
        }
    }
    if (verbosity != "queryPlanner") {
        assert.eq(nReturned, kNumDocs, tojson(results));
        assert.eq(nExamined, kNumDocs, tojson(results));
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

    pipeline = [{$project: {a: 1}}, {$limit: 5}, {$sort: {a: 1}}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity);

    pipeline = [{$project: {_id: 1}}, {$limit: 5}];
    checkResults(coll.explain(verbosity).aggregate(pipeline), verbosity);
}
})();
