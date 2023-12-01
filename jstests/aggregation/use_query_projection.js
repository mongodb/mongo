// Tests that an aggregation that only needs a finite set of fields to do its computations can
// sometimes use the query system to provide a covered plan which only returns those fields without
// fetching the full document.
//
// Relies on the initial $match being pushed into the query system in order for the planner to
// consider an index scan, so the pipelines cannot be wrapped in facet stages.
// @tags: [
//   do_not_wrap_aggregations_in_facets,
// ]
import {
    aggPlanHasStage,
    getOptimizer,
    hasRejectedPlans,
    isAggregationPlan,
    isQueryPlan,
    planHasStage,
} from "jstests/libs/analyze_plan.js";

const coll = db.use_query_projection;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; ++i) {
    bulk.insert({_id: i, x: "string", a: -i, y: i % 2});
}
assert.commandWorked(bulk.execute());

function assertQueryCoversProjection({pipeline = [], options = {}} = {}) {
    const explainOutput = coll.explain().aggregate(pipeline, options);
    const optimizer = getOptimizer(explainOutput);

    assert(isQueryPlan(explainOutput), explainOutput);
    // TODO SERVER-77719: Ensure that all stages are defined for all optimizers.
    let stage = {"classic": "FETCH"};
    if (stage[optimizer]) {
        assert(!planHasStage(db, explainOutput, stage[optimizer]), explainOutput);
    }
    // TODO SERVER-77719: Ensure that all stages are defined for all optimizers.
    stage = {"classic": "IXSCAN"};
    if (stage[optimizer]) {
        assert(planHasStage(db, explainOutput, stage[optimizer]), explainOutput);
    }

    switch (optimizer) {
        case "classic":
            assert(!hasRejectedPlans(explainOutput), explainOutput);
            break;
        case "CQF":
            // TODO SERVER-77719: Address the existence of rejected plans in CQF.
            break;
    }
    return explainOutput;
}

function assertQueryDoesNotCoverProjection({pipeline = []} = {}) {
    const explainOutput = coll.explain().aggregate(pipeline);
    const optimizer = getOptimizer(explainOutput);

    assert(isQueryPlan(explainOutput), explainOutput);
    // TODO SERVER-77719: Ensure that all stages are defined for all optimizers.
    let stage1 = {"classic": "FETCH"};
    let stage2 = {"classic": "COLLSCAN"};
    if (stage1[optimizer] && stage2[optimizer]) {
        assert(planHasStage(db, explainOutput, stage1[optimizer]) ||
                   aggPlanHasStage(stage2[optimizer]),
               explainOutput);
    }

    switch (optimizer) {
        case "classic":
            assert(!hasRejectedPlans(explainOutput), explainOutput);
            break;
        case "CQF":
            // TODO SERVER-77719: Address the existence of rejected plans in CQF.
            break;
    }
    return explainOutput;
}

assert.commandWorked(coll.createIndex({x: 1, a: -1, _id: 1}));

// Test that a pipeline requiring a subset of the fields in a compound index can use that index
// to cover the query.
assertQueryCoversProjection({pipeline: [{$match: {x: "string"}}, {$project: {_id: 1, x: 1}}]});
assertQueryCoversProjection({pipeline: [{$match: {x: "string"}}, {$project: {_id: 0, x: 1}}]});
assertQueryCoversProjection(
    {pipeline: [{$match: {x: "string"}}, {$project: {_id: 0, x: 1, a: 1}}]});
assertQueryCoversProjection(
    {pipeline: [{$match: {x: "string"}}, {$project: {_id: 1, x: 1, a: 1}}]});
assertQueryCoversProjection({
    pipeline: [{$match: {_id: 0, x: "string"}}, {$project: {_id: 1, x: 1, a: 1}}],
    options: {hint: {x: 1, a: -1, _id: 1}}
});

// Test that a pipeline requiring a field that is not in the index cannot use a covered plan.
assertQueryDoesNotCoverProjection({pipeline: [{$match: {x: "string"}}, {$project: {notThere: 1}}]});

// Test that a covered plan is the only plan considered, even if another plan would be equally
// selective. Add an equally selective index, then rely on assertQueryCoversProjection() to
// assert that there is only one considered plan, and it is a covered plan.
assert.commandWorked(coll.createIndex({x: 1}));
assertQueryCoversProjection({
    pipeline: [
        {$match: {_id: 0, x: "string"}},
        {$sort: {x: 1, a: 1}},  // Note: not indexable, but doesn't add any additional dependencies.
        {$project: {_id: 1, x: 1, a: 1}},
    ],
    options: {hint: {x: 1, a: -1, _id: 1}}
});

// Test that a multikey index will prevent a covered plan.
assert.commandWorked(coll.dropIndex({x: 1}));  // Make sure there is only one plan considered.
assert.commandWorked(coll.insert({x: ["an", "array!"]}));
assertQueryDoesNotCoverProjection(
    {pipeline: [{$match: {x: "string"}}, {$project: {_id: 1, x: 1}}]});
assertQueryDoesNotCoverProjection(
    {pipeline: [{$match: {x: "string"}}, {$project: {_id: 1, x: 1, a: 1}}]});
