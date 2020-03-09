/**
 * Tests aggregation on views for proper pipeline concatenation and semantics.
 *
 * The conditions under which sorts are pushed down were changed between 4.2 and 4.4. This test
 * expects the 4.4 version output of explain().
 * @tags: [requires_find_command,
 *         does_not_support_stepdowns,
 *         requires_getmore,
 *         requires_non_retryable_commands,
 *         # Requires FCV 4.4 because the test checks explain() output, and in 4.4 the conditions
 *         # under which sorts are pushed down were changed. Also uses $unionWith.
 *         requires_fcv_44,
 *         uses_$out]
 */
(function() {
"use strict";

// For assertMergeFailsForAllModesWithCode.
load("jstests/aggregation/extras/merge_helpers.js");
load("jstests/aggregation/extras/utils.js");  // For arrayEq, assertErrorCode, and
                                              // orderedArrayEq.
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.

let viewsDB = db.getSiblingDB("views_aggregation");
assert.commandWorked(viewsDB.dropDatabase());

// Helper functions.
let assertAggResultEq = function(collection, pipeline, expected, ordered) {
    let coll = viewsDB.getCollection(collection);
    let arr = coll.aggregate(pipeline).toArray();
    let success = (typeof (ordered) === "undefined" || !ordered) ? arrayEq(arr, expected)
                                                                 : orderedArrayEq(arr, expected);
    assert(success, tojson({got: arr, expected: expected}));
};
let byPopulation = function(a, b) {
    return a.pop - b.pop;
};

// Populate a collection with some test data.
const allDocuments = [
    {_id: "New York", state: "NY", pop: 7},
    {_id: "Newark", state: "NJ", pop: 3},
    {_id: "Palo Alto", state: "CA", pop: 10},
    {_id: "San Francisco", state: "CA", pop: 4},
    {_id: "Trenton", state: "NJ", pop: 5},
];

let coll = viewsDB.coll;
assert.commandWorked(coll.insert(allDocuments));

// Create views on the data.
assert.commandWorked(viewsDB.runCommand({create: "emptyPipelineView", viewOn: "coll"}));
assert.commandWorked(
    viewsDB.runCommand({create: "identityView", viewOn: "coll", pipeline: [{$match: {}}]}));
assert.commandWorked(viewsDB.runCommand(
    {create: "noIdView", viewOn: "coll", pipeline: [{$project: {_id: 0, state: 1, pop: 1}}]}));
assert.commandWorked(viewsDB.runCommand({
    create: "popSortedView",
    viewOn: "identityView",
    pipeline: [{$match: {pop: {$gte: 0}}}, {$sort: {pop: 1}}]
}));

(function testBasicAggregations() {
    // Find all documents with empty aggregations.
    assertAggResultEq("emptyPipelineView", [], allDocuments);
    assertAggResultEq("identityView", [], allDocuments);
    assertAggResultEq("identityView", [{$match: {}}], allDocuments);

    // Filter documents on a view with $match.
    assertAggResultEq(
        "popSortedView", [{$match: {state: "NY"}}], [{_id: "New York", state: "NY", pop: 7}]);

    // An aggregation still works on a view that strips _id.
    assertAggResultEq("noIdView", [{$match: {state: "NY"}}], [{state: "NY", pop: 7}]);

    // Aggregations work on views that sort.
    const doOrderedSort = true;
    assertAggResultEq("popSortedView", [], allDocuments.sort(byPopulation), doOrderedSort);
    assertAggResultEq("popSortedView", [{$limit: 1}, {$project: {_id: 1}}], [{_id: "Palo Alto"}]);
})();

(function testAggStagesWritingToViews() {
    // Test that the $out stage errors when writing to a view namespace.
    assertErrorCode(coll, [{$out: "emptyPipelineView"}], ErrorCodes.CommandNotSupportedOnView);

    // Test that the $merge stage errors when writing to a view namespace.
    assertMergeFailsForAllModesWithCode({
        source: viewsDB.coll,
        target: viewsDB.emptyPipelineView,
        errorCodes: [ErrorCodes.CommandNotSupportedOnView]
    });

    // Test that the $merge stage errors when writing to a view namespace in a foreign database.
    let foreignDB = db.getSiblingDB("views_aggregation_foreign");
    foreignDB.view.drop();
    assert.commandWorked(foreignDB.createView("view", "coll", []));

    assertMergeFailsForAllModesWithCode({
        source: viewsDB.coll,
        target: foreignDB.view,
        errorCodes: [ErrorCodes.CommandNotSupportedOnView]
    });
})();

(function testOptionsForwarding() {
    // Test that an aggregate on a view propagates the 'bypassDocumentValidation' option.
    const validatedCollName = "collectionWithValidator";
    viewsDB[validatedCollName].drop();
    assert.commandWorked(
        viewsDB.createCollection(validatedCollName, {validator: {illegalField: {$exists: false}}}));

    viewsDB.invalidDocs.drop();
    viewsDB.invalidDocsView.drop();
    assert.commandWorked(viewsDB.invalidDocs.insert({illegalField: "present"}));
    assert.commandWorked(viewsDB.createView("invalidDocsView", "invalidDocs", []));

    assert.commandWorked(
        viewsDB.runCommand({
            aggregate: "invalidDocsView",
            pipeline: [{$out: validatedCollName}],
            cursor: {},
            bypassDocumentValidation: true
        }),
        "Expected $out insertions to succeed since 'bypassDocumentValidation' was specified");

    // Test that an aggregate on a view propagates the 'allowDiskUse' option.
    const extSortLimit = 100 * 1024 * 1024;
    const largeStrSize = 10 * 1024 * 1024;
    const largeStr = new Array(largeStrSize).join('x');
    viewsDB.largeColl.drop();
    for (let i = 0; i <= extSortLimit / largeStrSize; ++i) {
        assert.commandWorked(viewsDB.largeColl.insert({x: i, largeStr: largeStr}));
    }
    assertErrorCode(viewsDB.largeColl,
                    [{$sort: {x: -1}}],
                    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
                    "Expected in-memory sort to fail due to excessive memory usage");
    viewsDB.largeView.drop();
    assert.commandWorked(viewsDB.createView("largeView", "largeColl", []));
    assertErrorCode(viewsDB.largeView,
                    [{$sort: {x: -1}}],
                    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
                    "Expected in-memory sort to fail due to excessive memory usage");

    assert.commandWorked(
        viewsDB.runCommand(
            {aggregate: "largeView", pipeline: [{$sort: {x: -1}}], cursor: {}, allowDiskUse: true}),
        "Expected aggregate to succeed since 'allowDiskUse' was specified");
})();

// Test explain modes on a view.
(function testExplainOnView() {
    let explainPlan = assert.commandWorked(
        viewsDB.popSortedView.explain("queryPlanner").aggregate([{$limit: 1}, {$match: {pop: 3}}]));
    assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace,
              "views_aggregation.coll",
              explainPlan);
    assert(!explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"), explainPlan);

    explainPlan = assert.commandWorked(viewsDB.popSortedView.explain("executionStats")
                                           .aggregate([{$limit: 1}, {$match: {pop: 3}}]));
    assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace,
              "views_aggregation.coll",
              explainPlan);
    assert(explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"), explainPlan);
    assert.eq(explainPlan.stages[0].$cursor.executionStats.nReturned, 1, explainPlan);
    assert(!explainPlan.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"),
           explainPlan);

    explainPlan = assert.commandWorked(viewsDB.popSortedView.explain("allPlansExecution")
                                           .aggregate([{$limit: 1}, {$match: {pop: 3}}]));
    assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace,
              "views_aggregation.coll",
              explainPlan);
    assert(explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"), explainPlan);
    assert.eq(explainPlan.stages[0].$cursor.executionStats.nReturned, 1, explainPlan);
    assert(explainPlan.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"),
           explainPlan);

    // Passing a value of true for the explain option to the aggregation command, without using the
    // shell explain helper, should continue to work.
    explainPlan = assert.commandWorked(
        viewsDB.popSortedView.aggregate([{$limit: 1}, {$match: {pop: 3}}], {explain: true}));
    assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace,
              "views_aggregation.coll",
              explainPlan);
    assert(!explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"), explainPlan);

    // Test allPlansExecution explain mode on the base collection.
    explainPlan = assert.commandWorked(
        viewsDB.coll.explain("allPlansExecution").aggregate([{$limit: 1}, {$match: {pop: 3}}]));
    assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace,
              "views_aggregation.coll",
              explainPlan);
    assert(explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"), explainPlan);
    assert.eq(explainPlan.stages[0].$cursor.executionStats.nReturned, 1, explainPlan);
    assert(explainPlan.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"),
           explainPlan);

    // The explain:true option should not work when paired with the explain shell helper.
    assert.throws(function() {
        viewsDB.popSortedView.explain("executionStats")
            .aggregate([{$limit: 1}, {$match: {pop: 3}}], {explain: true});
    });
})();

(
    function testLookupAndGraphLookup() {
        // We cannot lookup into sharded collections, so skip these tests if running in a sharded
        // configuration.
        if (FixtureHelpers.isMongos(db)) {
            jsTest.log(
                "Tests are being run on a mongos; skipping all $lookup and $graphLookup tests.");
            return;
        }

        // Test that the $lookup stage resolves the view namespace referenced in the 'from' field.
        assertAggResultEq(
        coll.getName(),
        [
            {$match: {_id: "New York"}},
            {$lookup: {from: "identityView", localField: "_id", foreignField: "_id", as: "matched"}},
            {$unwind: "$matched"},
            {$project: {_id: 1, matchedId: "$matched._id"}}
        ],
        [{_id: "New York", matchedId: "New York"}]);

        // Test that the $graphLookup stage resolves the view namespace referenced in the 'from'
        // field.
        assertAggResultEq(coll.getName(),
                      [
                        {$match: {_id: "New York"}},
                        {
                          $graphLookup: {
                              from: "identityView",
                              startWith: "$_id",
                              connectFromField: "_id",
                              connectToField: "_id",
                              as: "matched"
                          }
                        },
                        {$unwind: "$matched"},
                        {$project: {_id: 1, matchedId: "$matched._id"}}
                      ],
                      [{_id: "New York", matchedId: "New York"}]);

        // Test that the $lookup stage resolves the view namespace referenced in the 'from' field of
        // another $lookup stage nested inside of it.
        assert.commandWorked(viewsDB.runCommand({
    create: "viewWithLookupInside",
    viewOn: coll.getName(),
    pipeline: [
        {$lookup: {from: "identityView", localField: "_id", foreignField: "_id", as: "matched"}},
        {$unwind: "$matched"},
        {$project: {_id: 1, matchedId: "$matched._id"}}
    ]
}));

        assertAggResultEq(
        coll.getName(),
        [
          {$match: {_id: "New York"}},
          {
            $lookup: {
                from: "viewWithLookupInside",
                localField: "_id",
                foreignField: "matchedId",
                as: "matched"
            }
          },
          {$unwind: "$matched"},
          {$project: {_id: 1, matchedId1: "$matched._id", matchedId2: "$matched.matchedId"}}
        ],
        [{_id: "New York", matchedId1: "New York", matchedId2: "New York"}]);

        // Test that the $graphLookup stage resolves the view namespace referenced in the 'from'
        // field of a $lookup stage nested inside of it.
        let graphLookupPipeline = [
        {$match: {_id: "New York"}},
        {
          $graphLookup: {
              from: "viewWithLookupInside",
              startWith: "$_id",
              connectFromField: "_id",
              connectToField: "matchedId",
              as: "matched"
          }
        },
        {$unwind: "$matched"},
        {$project: {_id: 1, matchedId1: "$matched._id", matchedId2: "$matched.matchedId"}}
    ];

        assertAggResultEq(coll.getName(),
                          graphLookupPipeline,
                          [{_id: "New York", matchedId1: "New York", matchedId2: "New York"}]);

        // Test that the $lookup stage on a view with a nested $lookup on a different view resolves
        // the view namespaces referenced in their respective 'from' fields.
        assertAggResultEq(
        coll.getName(),
        [
          {$match: {_id: "Trenton"}},
          {$project: {state: 1}},
          {
            $lookup: {
                from: "identityView",
                as: "lookup1",
                pipeline: [
                    {$match: {_id: "Trenton"}},
                    {$project: {state: 1}},
                    {$lookup: {from: "popSortedView", as: "lookup2", pipeline: []}}
                ]
            }
          }
        ],
        [{
           "_id": "Trenton",
           "state": "NJ",
           "lookup1": [{
               "_id": "Trenton",
               "state": "NJ",
               "lookup2": [
                   {"_id": "Newark", "state": "NJ", "pop": 3},
                   {"_id": "San Francisco", "state": "CA", "pop": 4},
                   {"_id": "Trenton", "state": "NJ", "pop": 5},
                   {"_id": "New York", "state": "NY", "pop": 7},
                   {"_id": "Palo Alto", "state": "CA", "pop": 10}
               ]
           }]
        }]);

        // Test that the $facet stage resolves the view namespace referenced in the 'from' field of
        // a $lookup stage nested inside of a $graphLookup stage.
        assertAggResultEq(
            coll.getName(),
            [{$facet: {nested: graphLookupPipeline}}],
            [{nested: [{_id: "New York", matchedId1: "New York", matchedId2: "New York"}]}]);
    })();

(function testUnionReadFromView() {
    assert.eq(allDocuments.length, coll.aggregate([]).itcount());
    assert.eq(2 * allDocuments.length,
              coll.aggregate([{$unionWith: "emptyPipelineView"}]).itcount());
    assert.eq(2 * allDocuments.length, coll.aggregate([{$unionWith: "identityView"}]).itcount());
    assert.eq(
        2 * allDocuments.length,
        coll.aggregate(
                [{$unionWith: {coll: "noIdView", pipeline: [{$match: {_id: {$exists: false}}}]}}])
            .itcount());
    assert.eq(
        allDocuments.length + 1,
        coll.aggregate(
                [{$unionWith: {coll: "identityView", pipeline: [{$match: {_id: "New York"}}]}}])
            .itcount());
}());

(function testUnionInViewDefinition() {
    const secondCollection = viewsDB.secondCollection;
    secondCollection.drop();
    assert.commandWorked(secondCollection.insert(allDocuments));
    const viewName = "unionView";

    // Test with a simple $unionWith with no custom pipeline.
    assert.commandWorked(viewsDB.runCommand({
        create: viewName,
        viewOn: coll.getName(),
        pipeline: [{$unionWith: secondCollection.getName()}]
    }));
    assert.eq(2 * allDocuments.length, viewsDB[viewName].find().itcount());
    assert.eq(allDocuments.length,
              viewsDB[viewName].aggregate([{$group: {_id: "$_id"}}]).itcount());
    assert.eq(
        [
            {_id: "New York"},
            {_id: "Newark"},
            {_id: "Palo Alto"},
            {_id: "San Francisco"},
            {_id: "Trenton"}
        ],
        viewsDB[viewName].aggregate([{$group: {_id: "$_id"}}, {$sort: {_id: 1}}]).toArray());
    assert.eq(allDocuments.length, viewsDB[viewName].distinct("_id").length);
    viewsDB[viewName].drop();

    // Now test again with a custom pipeline in the view definition.
    assert.commandWorked(viewsDB.runCommand({
        create: viewName,
        viewOn: coll.getName(),
        pipeline:
            [{$unionWith: {coll: secondCollection.getName(), pipeline: [{$match: {state: "NY"}}]}}]
    }));
    assert.eq(allDocuments.length + 1, viewsDB[viewName].find().itcount());
    assert.eq(allDocuments.length,
              viewsDB[viewName].aggregate([{$group: {_id: "$_id"}}]).itcount());
    assert.eq(allDocuments.length, viewsDB[viewName].distinct("_id").length);
})();
})();
