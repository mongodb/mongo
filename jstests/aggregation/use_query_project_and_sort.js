// Tests that an aggregation that only needs a finite set of fields to do its computations, and has
// a $sort stage near the front of the pipeline can use the query system to provide a covered plan
// which only returns those fields, in the desired order, without fetching the full document.
//
// Relies on the ability to push leading $sorts down to the query system, so cannot wrap pipelines
// in $facet stages:
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For 'aggPlanHasStage' and other explain helpers.

    const coll = db.use_query_project_and_sort;
    coll.drop();

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; ++i) {
        bulk.insert({_id: i, x: "string", a: -i, y: i % 2});
    }
    assert.writeOK(bulk.execute());

    function assertQueryCoversProjectionAndSort(pipeline) {
        const explainOutput = coll.explain().aggregate(pipeline);
        assert(!aggPlanHasStage(explainOutput, "FETCH"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to include a FETCH stage in the explain output: " +
                   tojson(explainOutput));
        assert(!aggPlanHasStage(explainOutput, "SORT"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to include a SORT stage in the explain output: " +
                   tojson(explainOutput));
        assert(!aggPlanHasStage(explainOutput, "$sort"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to include a SORT stage in the explain output: " +
                   tojson(explainOutput));
        assert(aggPlanHasStage(explainOutput, "IXSCAN"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " to include an index scan in the explain output: " + tojson(explainOutput));
        assert(!hasRejectedPlans(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " not to have any rejected plans in the explain output: " +
                   tojson(explainOutput));
        return explainOutput;
    }

    assert.commandWorked(coll.createIndex({x: 1, a: -1, _id: 1}));

    // Test that a pipeline requiring a subset of the fields in a compound index can use that index
    // to cover the query.
    assertQueryCoversProjectionAndSort(
        [{$match: {x: "string"}}, {$sort: {x: 1}}, {$project: {_id: 0, x: 1}}]);
    assertQueryCoversProjectionAndSort(
        [{$match: {x: "string"}}, {$sort: {x: 1}}, {$project: {_id: 1, x: 1}}]);
    assertQueryCoversProjectionAndSort(
        [{$match: {x: "string"}}, {$sort: {x: -1, a: 1}}, {$project: {_id: 1, x: 1}}]);
    assertQueryCoversProjectionAndSort(
        [{$match: {x: "string"}}, {$sort: {x: 1, a: -1, _id: 1}}, {$project: {_id: 1}}]);
    assertQueryCoversProjectionAndSort(
        [{$match: {x: "string"}}, {$sort: {x: 1, a: -1, _id: 1}}, {$project: {_id: 1, x: 1}}]);
    assertQueryCoversProjectionAndSort(
        [{$match: {x: "string"}}, {$sort: {x: 1, a: -1, _id: 1}}, {$project: {_id: 1, a: 1}}]);
    assertQueryCoversProjectionAndSort([
        {$match: {x: "string"}},
        {$sort: {x: 1, a: -1, _id: 1}},
        {$project: {_id: 0, a: 1, x: 1}}
    ]);
    assertQueryCoversProjectionAndSort([
        {$match: {x: "string"}},
        {$sort: {x: 1, a: -1, _id: 1}},
        {$project: {_id: 1, x: 1, a: 1}}
    ]);
}());
