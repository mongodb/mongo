// Tests that an aggregation that only needs a finite set of fields to do its computations can
// sometimes use the query system to provide a covered plan which only returns those fields without
// fetching the full document.
//
// Relies on the initial $match being pushed into the query system in order for the planner to
// consider an index scan, so the pipelines cannot be wrapped in facet stages.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For 'aggPlanHasStage' and other explain helpers.

    const coll = db.use_query_projection;
    coll.drop();

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; ++i) {
        bulk.insert({_id: i, x: "string", a: -i, y: i % 2});
    }
    assert.writeOK(bulk.execute());

    function assertQueryCoversProjection({pipeline = [], pipelineOptimizedAway = true} = {}) {
        const explainOutput = coll.explain().aggregate(pipeline);

        if (pipelineOptimizedAway) {
            assert(isQueryPlan(explainOutput));
            assert(!planHasStage(db, explainOutput, "FETCH"),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " *not* to include a FETCH stage in the explain output: " +
                       tojson(explainOutput));
            assert(planHasStage(db, explainOutput, "IXSCAN"),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " to include an index scan in the explain output: " + tojson(explainOutput));
        } else {
            assert(isAggregationPlan(explainOutput));
            assert(!aggPlanHasStage(explainOutput, "FETCH"),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " *not* to include a FETCH stage in the explain output: " +
                       tojson(explainOutput));
            assert(aggPlanHasStage(explainOutput, "IXSCAN"),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " to include an index scan in the explain output: " + tojson(explainOutput));
        }
        assert(!hasRejectedPlans(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " not to have any rejected plans in the explain output: " +
                   tojson(explainOutput));
        return explainOutput;
    }

    function assertQueryDoesNotCoverProjection({pipeline = [], pipelineOptimizedAway = true} = {}) {
        const explainOutput = coll.explain().aggregate(pipeline);

        if (pipelineOptimizedAway) {
            assert(isQueryPlan(explainOutput));
            assert(planHasStage(db, explainOutput, "FETCH") || aggPlanHasStage("COLLSCAN"),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " to include a FETCH or COLLSCAN stage in the explain output: " +
                       tojson(explainOutput));
            assert(!hasRejectedPlans(explainOutput),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " not to have any rejected plans in the explain output: " +
                       tojson(explainOutput));
        } else {
            assert(isAggregationPlan(explainOutput));
            assert(aggPlanHasStage(explainOutput, "FETCH") || aggPlanHasStage("COLLSCAN"),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " to include a FETCH or COLLSCAN stage in the explain output: " +
                       tojson(explainOutput));
            assert(!hasRejectedPlans(explainOutput),
                   "Expected pipeline " + tojsononeline(pipeline) +
                       " not to have any rejected plans in the explain output: " +
                       tojson(explainOutput));
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
    assertQueryCoversProjection(
        {pipeline: [{$match: {_id: 0, x: "string"}}, {$project: {_id: 1, x: 1, a: 1}}]});

    // Test that a pipeline requiring a field that is not in the index cannot use a covered plan.
    assertQueryDoesNotCoverProjection({
        pipeline: [{$match: {x: "string"}}, {$project: {notThere: 1}}],
        pipelineOptimizedAway: false
    });

    // Test that a covered plan is the only plan considered, even if another plan would be equally
    // selective. Add an equally selective index, then rely on assertQueryCoversProjection() to
    // assert that there is only one considered plan, and it is a covered plan.
    assert.commandWorked(coll.createIndex({x: 1}));
    assertQueryCoversProjection({
        pipeline: [
            {$match: {_id: 0, x: "string"}},
            {
              $sort: {
                  x: 1,
                  a: 1
              }
            },  // Note: not indexable, but doesn't add any additional dependencies.
            {$project: {_id: 1, x: 1, a: 1}},
        ],
        pipelineOptimizedAway: false
    });

    // Test that a multikey index will prevent a covered plan.
    assert.commandWorked(coll.dropIndex({x: 1}));  // Make sure there is only one plan considered.
    assert.writeOK(coll.insert({x: ["an", "array!"]}));
    assertQueryDoesNotCoverProjection({
        pipeline: [{$match: {x: "string"}}, {$project: {_id: 1, x: 1}}],
        pipelineOptimizedAway: false
    });
    assertQueryDoesNotCoverProjection({
        pipeline: [{$match: {x: "string"}}, {$project: {_id: 1, x: 1, a: 1}}],
        pipelineOptimizedAway: false
    });
}());
