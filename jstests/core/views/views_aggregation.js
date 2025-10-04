/**
 * Tests aggregation on views for proper pipeline concatenation and semantics.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   # The killCursors command is not allowed with a security token.
 *   not_allowed_with_signed_security_token,
 *   requires_getmore,
 *   requires_non_retryable_commands,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   references_foreign_collection,
 *   # Some of the SBE statistics we check have different values pre-7.1, and this test only checks
 *   # the values that we expect from the 7.2-and-later SBE.
 *   requires_fcv_72,
 * ]
 */

// TODO SERVER-92452: This test fails in burn-in with the 'inMemory' engine with the 'WT_CACHE_FULL'
// error. This is a known issue and can be ignored. Remove this comment once SERVER-92452 is fixed.

import {assertMergeFailsForAllModesWithCode} from "jstests/aggregation/extras/merge_helpers.js";
import {arrayEq, assertErrorCode, orderedArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js"; // For arrayEq, assertErrorCode, and
import {getEngine, getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";

let viewsDB = db.getSiblingDB("views_aggregation");
assert.commandWorked(viewsDB.dropDatabase());

// Helper functions.
let assertAggResultEq = function (collection, pipeline, expected, ordered) {
    let coll = viewsDB.getCollection(collection);
    let arr = coll.aggregate(pipeline).toArray();
    let success = typeof ordered === "undefined" || !ordered ? arrayEq(arr, expected) : orderedArrayEq(arr, expected);
    assert(success, tojson({got: arr, expected: expected}));
};
let byPopulation = function (a, b) {
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
assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: "coll", pipeline: [{$match: {}}]}));
assert.commandWorked(
    viewsDB.runCommand({create: "noIdView", viewOn: "coll", pipeline: [{$project: {_id: 0, state: 1, pop: 1}}]}),
);
assert.commandWorked(
    viewsDB.runCommand({
        create: "popSortedView",
        viewOn: "identityView",
        pipeline: [{$match: {pop: {$gte: 0}}}, {$sort: {pop: 1}}],
    }),
);

(function testBasicAggregations() {
    // Find all documents with empty aggregations.
    assertAggResultEq("emptyPipelineView", [], allDocuments);
    assertAggResultEq("identityView", [], allDocuments);
    assertAggResultEq("identityView", [{$match: {}}], allDocuments);

    // Filter documents on a view with $match.
    assertAggResultEq("popSortedView", [{$match: {state: "NY"}}], [{_id: "New York", state: "NY", pop: 7}]);

    // An aggregation still works on a view that strips _id.
    assertAggResultEq("noIdView", [{$match: {state: "NY"}}], [{state: "NY", pop: 7}]);

    // Aggregations work on views that sort.
    const doOrderedSort = true;
    assertAggResultEq("popSortedView", [], allDocuments.sort(byPopulation), doOrderedSort);
    assertAggResultEq("popSortedView", [{$limit: 1}, {$project: {_id: 1}}], [{_id: "Newark"}]);
})();

(function testAggStagesWritingToViews() {
    // Test that the $out stage errors when writing to a view namespace.
    assertErrorCode(coll, [{$out: "emptyPipelineView"}], ErrorCodes.CommandNotSupportedOnView);

    // Test that the $merge stage errors when writing to a view namespace.
    assertMergeFailsForAllModesWithCode({
        source: viewsDB.coll,
        target: viewsDB.emptyPipelineView,
        errorCodes: [ErrorCodes.CommandNotSupportedOnView],
    });

    // Test that the $merge stage errors when writing to a view namespace in a foreign database.
    let foreignDB = db.getSiblingDB("views_aggregation_foreign");
    foreignDB.view.drop();
    assert.commandWorked(foreignDB.createView("view", "coll", []));

    assertMergeFailsForAllModesWithCode({
        source: viewsDB.coll,
        target: foreignDB.view,
        errorCodes: [ErrorCodes.CommandNotSupportedOnView],
    });
})();

(function testOptionsForwarding() {
    // Test that an aggregate on a view propagates the 'bypassDocumentValidation' option.
    const validatedCollName = "collectionWithValidator";
    viewsDB[validatedCollName].drop();
    assert.commandWorked(viewsDB.createCollection(validatedCollName, {validator: {illegalField: {$exists: false}}}));

    viewsDB.invalidDocs.drop();
    viewsDB.invalidDocsView.drop();
    assert.commandWorked(viewsDB.invalidDocs.insert({illegalField: "present"}));
    assert.commandWorked(viewsDB.createView("invalidDocsView", "invalidDocs", []));

    assert.commandWorked(
        viewsDB.runCommand({
            aggregate: "invalidDocsView",
            pipeline: [{$out: validatedCollName}],
            cursor: {},
            bypassDocumentValidation: true,
        }),
        "Expected $out insertions to succeed since 'bypassDocumentValidation' was specified",
    );

    // Test that an aggregate on a view propagates the 'allowDiskUse' option.
    const extSortLimit = 100 * 1024 * 1024;
    const largeStrSize = 10 * 1024 * 1024;
    const largeStr = "x".repeat(largeStrSize);
    viewsDB.largeColl.drop();
    for (let i = 0; i <= extSortLimit / largeStrSize; ++i) {
        assert.commandWorked(viewsDB.largeColl.insert({x: i, largeStr: largeStr}));
    }
    assertErrorCode(
        viewsDB.largeColl,
        [{$sort: {x: -1}}],
        ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
        "Expected in-memory sort to fail due to excessive memory usage",
        {allowDiskUse: false},
    );
    viewsDB.largeView.drop();
    assert.commandWorked(viewsDB.createView("largeView", "largeColl", []));
    assertErrorCode(
        viewsDB.largeView,
        [{$sort: {x: -1}}],
        ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
        "Expected in-memory sort to fail due to excessive memory usage",
        {allowDiskUse: false},
    );

    const result1 = assert.commandWorked(
        viewsDB.runCommand({aggregate: "largeView", pipeline: [{$sort: {x: -1}}], cursor: {}, allowDiskUse: true}),
        "Expected aggregate to succeed since 'allowDiskUse' was specified",
    );

    const result2 = assert.commandWorked(
        viewsDB.runCommand({aggregate: "largeView", pipeline: [{$sort: {x: -1}}], cursor: {}}),
        "Expected aggregate to succeed since 'allowDiskUse' is true by default",
    );

    // These pipelines can consume significant memory and disk space, so we manually close them to
    // prevent them from interfering with other tests. We ignore the return value here, because an
    // error closing cursors does not usually represent a failure.
    viewsDB.runCommand({killCursors: "largeView", cursors: [result1.cursor.id, result2.cursor.id]});
})();

// Test explain modes on a view.
(function testExplainOnView() {
    let explainPlan = assert.commandWorked(
        viewsDB.popSortedView.explain("queryPlanner").aggregate([{$limit: 1}, {$match: {pop: 3}}]),
    );
    explainPlan = getSingleNodeExplain(explainPlan);
    if (explainPlan.hasOwnProperty("stages")) {
        explainPlan = explainPlan.stages[0].$cursor;
    }
    assert.eq(explainPlan.queryPlanner.namespace, "views_aggregation.coll", explainPlan);
    assert(!explainPlan.hasOwnProperty("executionStats"), explainPlan);

    explainPlan = assert.commandWorked(
        viewsDB.popSortedView.explain("executionStats").aggregate([{$limit: 1}, {$match: {pop: 3}}]),
    );
    explainPlan = getSingleNodeExplain(explainPlan);
    if (explainPlan.hasOwnProperty("stages")) {
        explainPlan = explainPlan.stages[0].$cursor;
    }
    assert.eq(explainPlan.queryPlanner.namespace, "views_aggregation.coll", explainPlan);
    assert(explainPlan.hasOwnProperty("executionStats"), explainPlan);
    assert.eq(explainPlan.executionStats.nReturned, 1, explainPlan);
    assert(!explainPlan.executionStats.hasOwnProperty("allPlansExecution"), explainPlan);

    explainPlan = assert.commandWorked(
        viewsDB.popSortedView.explain("allPlansExecution").aggregate([{$limit: 1}, {$match: {pop: 3}}]),
    );
    explainPlan = getSingleNodeExplain(explainPlan);
    if (explainPlan.hasOwnProperty("stages")) {
        explainPlan = explainPlan.stages[0].$cursor;
    }
    assert.eq(explainPlan.queryPlanner.namespace, "views_aggregation.coll", explainPlan);
    assert(explainPlan.hasOwnProperty("executionStats"), explainPlan);
    assert.eq(explainPlan.executionStats.nReturned, 1, explainPlan);
    assert(explainPlan.executionStats.hasOwnProperty("allPlansExecution"), explainPlan);

    // Passing a value of true for the explain option to the aggregation command, without using the
    // shell explain helper, should continue to work.
    explainPlan = assert.commandWorked(
        viewsDB.popSortedView.aggregate([{$limit: 1}, {$match: {pop: 3}}], {explain: true}),
    );
    explainPlan = getSingleNodeExplain(explainPlan);
    if (explainPlan.hasOwnProperty("stages")) {
        explainPlan = explainPlan.stages[0].$cursor;
    }
    assert.eq(explainPlan.queryPlanner.namespace, "views_aggregation.coll", explainPlan);
    assert(!explainPlan.hasOwnProperty("executionStats"), explainPlan);

    // Test allPlansExecution explain mode on the base collection.
    explainPlan = assert.commandWorked(
        viewsDB.coll.explain("allPlansExecution").aggregate([{$limit: 1}, {$match: {pop: 3}}]),
    );
    explainPlan = getSingleNodeExplain(explainPlan);
    if (explainPlan.hasOwnProperty("stages")) {
        explainPlan = explainPlan.stages[0].$cursor;
    }
    assert.eq(explainPlan.queryPlanner.namespace, "views_aggregation.coll", explainPlan);
    assert(explainPlan.hasOwnProperty("executionStats"), explainPlan);
    // In BF-36630/BF-36875, 'checkSbeFullyEnabled' reported SBE to be off, but the explain output
    // included a slotBasedPlan. Though we're unable to reproduce the bug, we can check the explain
    // for a slot based plain instead of using 'checkSbeFullyEnabled'.
    if (getEngine(explainPlan) == "sbe") {
        assert.eq(explainPlan.executionStats.nReturned, 0, explainPlan);
    } else {
        assert.eq(explainPlan.executionStats.nReturned, 1, explainPlan);
    }
    assert(explainPlan.executionStats.hasOwnProperty("allPlansExecution"), explainPlan);

    // The explain:true option should not work when paired with the explain shell helper.
    assert.throws(function () {
        viewsDB.popSortedView.explain("executionStats").aggregate([{$limit: 1}, {$match: {pop: 3}}], {explain: true});
    });
})();

(function testLookupAndGraphLookup() {
    // We cannot lookup into sharded collections, so skip these tests if running in a sharded
    // configuration.
    if (FixtureHelpers.isMongos(db)) {
        jsTest.log("Tests are being run on a mongos; skipping all $lookup and $graphLookup tests.");
        return;
    }

    // Test that the $lookup stage resolves the view namespace referenced in the 'from' field.
    assertAggResultEq(
        coll.getName(),
        [
            {$match: {_id: "New York"}},
            {$lookup: {from: "identityView", localField: "_id", foreignField: "_id", as: "matched"}},
            {$unwind: "$matched"},
            {$project: {_id: 1, matchedId: "$matched._id"}},
        ],
        [{_id: "New York", matchedId: "New York"}],
    );

    // Test that the $graphLookup stage resolves the view namespace referenced in the 'from'
    // field.
    assertAggResultEq(
        coll.getName(),
        [
            {$match: {_id: "New York"}},
            {
                $graphLookup: {
                    from: "identityView",
                    startWith: "$_id",
                    connectFromField: "_id",
                    connectToField: "_id",
                    as: "matched",
                },
            },
            {$unwind: "$matched"},
            {$project: {_id: 1, matchedId: "$matched._id"}},
        ],
        [{_id: "New York", matchedId: "New York"}],
    );

    // Test that the $lookup stage resolves the view namespace referenced in the 'from' field of
    // another $lookup stage nested inside of it.
    assert.commandWorked(
        viewsDB.runCommand({
            create: "viewWithLookupInside",
            viewOn: coll.getName(),
            pipeline: [
                {$lookup: {from: "identityView", localField: "_id", foreignField: "_id", as: "matched"}},
                {$unwind: "$matched"},
                {$project: {_id: 1, matchedId: "$matched._id"}},
            ],
        }),
    );

    assertAggResultEq(
        coll.getName(),
        [
            {$match: {_id: "New York"}},
            {
                $lookup: {
                    from: "viewWithLookupInside",
                    localField: "_id",
                    foreignField: "matchedId",
                    as: "matched",
                },
            },
            {$unwind: "$matched"},
            {$project: {_id: 1, matchedId1: "$matched._id", matchedId2: "$matched.matchedId"}},
        ],
        [{_id: "New York", matchedId1: "New York", matchedId2: "New York"}],
    );

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
                as: "matched",
            },
        },
        {$unwind: "$matched"},
        {$project: {_id: 1, matchedId1: "$matched._id", matchedId2: "$matched.matchedId"}},
    ];

    assertAggResultEq(coll.getName(), graphLookupPipeline, [
        {_id: "New York", matchedId1: "New York", matchedId2: "New York"},
    ]);

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
                        {$lookup: {from: "popSortedView", as: "lookup2", pipeline: []}},
                    ],
                },
            },
        ],
        [
            {
                "_id": "Trenton",
                "state": "NJ",
                "lookup1": [
                    {
                        "_id": "Trenton",
                        "state": "NJ",
                        "lookup2": [
                            {"_id": "Newark", "state": "NJ", "pop": 3},
                            {"_id": "San Francisco", "state": "CA", "pop": 4},
                            {"_id": "Trenton", "state": "NJ", "pop": 5},
                            {"_id": "New York", "state": "NY", "pop": 7},
                            {"_id": "Palo Alto", "state": "CA", "pop": 10},
                        ],
                    },
                ],
            },
        ],
    );

    // Test that the $facet stage resolves the view namespace referenced in the 'from' field of
    // a $lookup stage nested inside of a $graphLookup stage.
    assertAggResultEq(
        coll.getName(),
        [{$facet: {nested: graphLookupPipeline}}],
        [{nested: [{_id: "New York", matchedId1: "New York", matchedId2: "New York"}]}],
    );
})();

(function testUnionReadFromView() {
    assert.eq(allDocuments.length, coll.aggregate([]).itcount());
    assert.eq(2 * allDocuments.length, coll.aggregate([{$unionWith: "emptyPipelineView"}]).itcount());
    assert.eq(2 * allDocuments.length, coll.aggregate([{$unionWith: "identityView"}]).itcount());
    assert.eq(
        2 * allDocuments.length,
        coll.aggregate([{$unionWith: {coll: "noIdView", pipeline: [{$match: {_id: {$exists: false}}}]}}]).itcount(),
    );
    assert.eq(
        allDocuments.length + 1,
        coll.aggregate([{$unionWith: {coll: "identityView", pipeline: [{$match: {_id: "New York"}}]}}]).itcount(),
    );
})();

(function testUnionInViewDefinition() {
    const secondCollection = viewsDB.secondCollection;
    secondCollection.drop();
    assert.commandWorked(secondCollection.insert(allDocuments));
    const viewName = "unionView";

    // Test with a simple $unionWith with no custom pipeline.
    assert.commandWorked(
        viewsDB.runCommand({
            create: viewName,
            viewOn: coll.getName(),
            pipeline: [{$unionWith: secondCollection.getName()}],
        }),
    );
    assert.eq(2 * allDocuments.length, viewsDB[viewName].find().itcount());
    assert.eq(allDocuments.length, viewsDB[viewName].aggregate([{$group: {_id: "$_id"}}]).itcount());
    assert.eq(
        [{_id: "New York"}, {_id: "Newark"}, {_id: "Palo Alto"}, {_id: "San Francisco"}, {_id: "Trenton"}],
        viewsDB[viewName].aggregate([{$group: {_id: "$_id"}}, {$sort: {_id: 1}}]).toArray(),
    );
    assert.eq(allDocuments.length, viewsDB[viewName].distinct("_id").length);
    viewsDB[viewName].drop();

    // Now test again with a custom pipeline in the view definition.
    assert.commandWorked(
        viewsDB.runCommand({
            create: viewName,
            viewOn: coll.getName(),
            pipeline: [{$unionWith: {coll: secondCollection.getName(), pipeline: [{$match: {state: "NY"}}]}}],
        }),
    );
    assert.eq(allDocuments.length + 1, viewsDB[viewName].find().itcount());
    assert.eq(allDocuments.length, viewsDB[viewName].aggregate([{$group: {_id: "$_id"}}]).itcount());
    assert.eq(allDocuments.length, viewsDB[viewName].distinct("_id").length);
})();
