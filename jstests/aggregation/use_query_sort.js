// Tests that an aggregation with a $sort near the front of the pipeline can sometimes use the query
// system to provide the sort.
//
// Relies on the ability to push leading $sorts down to the query system, so cannot wrap pipelines
// in $facet stages:
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");  // For 'aggPlanHasStage' and other explain helpers.

    const coll = db.use_query_sort;
    coll.drop();

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; ++i) {
        bulk.insert({_id: i, x: "string", a: -i, y: i % 2});
    }
    assert.writeOK(bulk.execute());

    function assertHasNonBlockingQuerySort(pipeline) {
        const explainOutput = coll.explain().aggregate(pipeline);
        assert(!aggPlanHasStage(explainOutput, "$sort"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to include a $sort stage in the explain output: " +
                   tojson(explainOutput));
        assert(!aggPlanHasStage(explainOutput, "SORT"),
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

    function assertDoesNotHaveQuerySort(pipeline) {
        const explainOutput = coll.explain().aggregate(pipeline);
        assert(aggPlanHasStage(explainOutput, "$sort"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " to include a $sort stage in the explain output: " + tojson(explainOutput));
        assert(!aggPlanHasStage(explainOutput, "SORT"),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to include a SORT stage in the explain output: " +
                   tojson(explainOutput));
        assert(!hasRejectedPlans(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " not to have any rejected plans in the explain output: " +
                   tojson(explainOutput));
        return explainOutput;
    }

    // Test that a sort on the _id can use the query system to provide the sort.
    assertHasNonBlockingQuerySort([{$sort: {_id: -1}}]);
    assertHasNonBlockingQuerySort([{$sort: {_id: 1}}]);
    assertHasNonBlockingQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {_id: 1}}]);
    assertHasNonBlockingQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {_id: -1}}]);

    // Test that a sort on a field not in any index cannot use a query system sort, and thus still
    // has a $sort stage.
    assertDoesNotHaveQuerySort([{$sort: {x: -1}}]);
    assertDoesNotHaveQuerySort([{$sort: {x: 1}}]);
    assertDoesNotHaveQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {x: 1}}]);

    assert.commandWorked(coll.createIndex({x: 1, y: -1}));

    assertHasNonBlockingQuerySort([{$sort: {x: 1, y: -1}}]);
    assertHasNonBlockingQuerySort([{$sort: {x: 1}}]);
    assertDoesNotHaveQuerySort([{$sort: {y: 1}}]);
    assertDoesNotHaveQuerySort([{$sort: {x: 1, y: 1}}]);

    // Test that a $match on a field not present in the same index eligible to provide a sort can
    // still result in a index scan on the sort field (SERVER-7568).
    assertHasNonBlockingQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {x: 1}}]);

    // Test that a sort on the text score does not use the query system to provide the sort, since
    // it would need to be a blocking sort, and we prefer the $sort stage to the query system's sort
    // implementation.
    assert.commandWorked(coll.createIndex({x: "text"}));
    assertDoesNotHaveQuerySort(
        [{$match: {$text: {$search: "test"}}}, {$sort: {key: {$meta: "textScore"}}}]);
}());
