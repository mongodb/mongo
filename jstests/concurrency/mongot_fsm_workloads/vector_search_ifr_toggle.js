/**
 * vector_search_ifr_toggle.js
 *
 * This workload pressure tests toggling the featureFlagVectorSearchExtension IFR flag while
 * running $vectorSearch queries concurrently. It exercises complex scenarios including:
 * - Basic $vectorSearch on collections
 * - $vectorSearch on views with transformation pipelines
 * - $vectorSearch inside $unionWith sub-pipelines
 * - Nested $unionWith with $vectorSearch on views
 *
 * The expectation is that everything should continue to work as expected as the feature flag
 * is toggled on or off, no matter which node(s) have the flag enabled/when.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   requires_fcv_83,
 * ]
 */
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {createSearchIndex} from "jstests/libs/search.js";

export const $config = (function () {
    // Test data configuration
    const kTestCollName = "vector_search_coll";
    const kViewName = "vector_search_view";
    const kNestedViewName = "vector_search_nested_view";
    const kUnionTargetCollName = "union_target_coll";
    const kIndexName = "test_vector_index";

    const kTestData = [
        {_id: 1, title: "Document One", embedding: [1.0, 0.0, 0.0, 0.0]},
        {_id: 2, title: "Document Two", embedding: [0.0, 1.0, 0.0, 0.0]},
        {_id: 3, title: "Document Three", embedding: [0.0, 0.0, 1.0, 0.0]},
        {_id: 4, title: "Document Four", embedding: [0.0, 0.0, 0.0, 1.0]},
        {_id: 5, title: "Document Five", embedding: [0.5, 0.5, 0.0, 0.0]},
        {_id: 6, title: "Document Six", embedding: [0.0, 0.5, 0.5, 0.0]},
        {_id: 7, title: "Document Seven", embedding: [0.0, 0.0, 0.5, 0.5]},
        {_id: 8, title: "Document Eight", embedding: [0.5, 0.0, 0.0, 0.5]},
        {_id: 9, title: "Document Nine", embedding: [0.5, 0.5, 0.5, 0.5]},
    ];

    const kViewPipeline = [{$addFields: {enriched: {$concat: ["$title", " - enriched"]}}}];

    const kNestedViewPipeline = [{$addFields: {nested: true}}];

    // Looks for a single document closest to the embedding [0.5, 0.5, 0.5, 0.5], which is document 9.
    const kVectorSearchQuery = {
        queryVector: [0.5, 0.5, 0.5, 0.5],
        path: "embedding",
        numCandidates: 10,
        limit: 1,
        index: kIndexName,
    };

    function assertExpectedResult(results) {
        assert.eq(results.length, 1, "Expected 1 result, got " + results.length);
        assert.eq(results[0]._id, 9, "Expected document 9, got " + results[0]._id);
        return results[0];
    }

    const kVectorSearchIndexSpec = {
        name: kIndexName,
        type: "vectorSearch",
        definition: {
            fields: [
                {
                    type: "vector",
                    numDimensions: 4,
                    path: "embedding",
                    similarity: "euclidean",
                },
            ],
        },
    };

    let data = {
        collName: kTestCollName,
        viewName: kViewName,
        nestedViewName: kNestedViewName,
        unionTargetCollName: kUnionTargetCollName,
        vectorSearchQuery: kVectorSearchQuery,
        testData: kTestData,
    };

    let states = {
        // noop, just for a place to start the phase diagram.
        init: function init(db, collName) {},

        // Note that our implementation here includes a 'turn it on everywhere' function and a 'turn
        // it off everywhere' function. Because these can race with each other, and also because
        // they do not atomically enable the flag everywhere at the same instant, we get the
        // coverage we want. We will end up intermixing queries which observe the flag on and off in
        // various combinations across the cluster.
        toggleFlagOn: function toggleFlagOn(db, collName) {
            setParameterOnAllNonConfigNodes(db.getMongo(), "featureFlagVectorSearchExtension", true);
        },
        toggleFlagOff: function toggleFlagOff(db, collName) {
            setParameterOnAllNonConfigNodes(db.getMongo(), "featureFlagVectorSearchExtension", false);
        },

        basicVectorSearch: function basicVectorSearch(db, collName) {
            const coll = db[this.collName];
            const pipeline = [{$vectorSearch: this.vectorSearchQuery}];

            const result = assertExpectedResult(coll.aggregate(pipeline).toArray());
            assert(!result.hasOwnProperty("enriched"), "Expected no view to be applied");
            assert(!result.hasOwnProperty("nested"), "Expected no nested view to be applied");
        },

        vectorSearchOnView: function vectorSearchOnView(db, collName) {
            const view = db[this.viewName];
            const pipeline = [{$vectorSearch: {...this.vectorSearchQuery, index: "view_index"}}];

            const result = assertExpectedResult(view.aggregate(pipeline).toArray());
            assert(result.hasOwnProperty("enriched"), "Expected view to be applied");
            assert(!result.hasOwnProperty("nested"), "Expected no nested view to be applied");
        },

        vectorSearchOnNestedView: function vectorSearchOnNestedView(db, collName) {
            const nestedView = db[kNestedViewName];
            const pipeline = [{$vectorSearch: {...this.vectorSearchQuery, index: "nested_view_index"}}];

            const result = assertExpectedResult(nestedView.aggregate(pipeline).toArray());
            assert(result.hasOwnProperty("nested"), "Expected nested view to be applied");
            assert(result.hasOwnProperty("enriched"), "Expected enriched view to be applied");
        },

        vectorSearchInUnionWith: function vectorSearchInUnionWith(db, collName) {
            const coll = db[this.collName];
            const pipeline = [
                {$match: {_id: {$lte: 3}}},
                {
                    $unionWith: {
                        coll: this.unionTargetCollName,
                        pipeline: [{$vectorSearch: this.vectorSearchQuery}],
                    },
                },
            ];

            const results = coll.aggregate(pipeline).toArray();
            assert.eq(3 /* _id <= 3 */ + 1 /* $unionWith result */, results.length);
        },

        vectorSearchInUnionWithOnView: function vectorSearchInUnionWithOnView(db, collName) {
            const coll = db[this.collName];
            // Run $vectorSearch on a view inside a $unionWith.
            const pipeline = [
                {$match: {_id: {$lte: 2}}},
                {
                    $unionWith: {
                        coll: this.viewName,
                        pipeline: [{$vectorSearch: {...this.vectorSearchQuery, index: "view_index"}}],
                    },
                },
            ];

            const results = coll.aggregate(pipeline).toArray();
            assert.eq(2 /* _id <= 2 */ + 1 /* $unionWith result */, results.length);
        },
        dualSearchOuterViewAndInnerView: function dualSearchOuterViewAndInnerView(db, collName) {
            const outerView = db[this.viewName];
            const pipeline = [
                {$vectorSearch: {...this.vectorSearchQuery, index: "view_index"}},
                {
                    $unionWith: {
                        coll: kNestedViewName,
                        pipeline: [
                            {$vectorSearch: {...this.vectorSearchQuery, index: "nested_view_index"}},
                            {$addFields: {my_nested: true}},
                        ],
                    },
                },
            ];

            const results = outerView.aggregate(pipeline).toArray();
            assert.eq(1 /* $vectorSearch result */ + 1 /* $unionWith result */, results.length);
            function hasEnriched(result) {
                return result.hasOwnProperty("enriched");
            }
            function hasNested(result) {
                return result.hasOwnProperty("nested");
            }
            // We can't assert on any particular order, but we know that one should be enriched and
            // one should be nested.
            assert(
                (hasEnriched(results[0]) && hasNested(results[1])) ||
                    (hasNested(results[0]) && hasEnriched(results[1])),
                results,
            );
        },
    };

    // Transitions weighted to run queries more frequently than flag toggles:
    // 90% queries, 10% flag toggles.
    let transitions = {
        init: {
            toggleFlagOn: 0.05,
            toggleFlagOff: 0.05,
            basicVectorSearch: 0.18,
            vectorSearchOnView: 0.18,
            vectorSearchOnNestedView: 0.18,
            vectorSearchInUnionWith: 0.18,
            vectorSearchInUnionWithOnView: 0.18,
            dualSearchOuterViewAndInnerView: 0.18,
        },
        toggleFlagOn: {
            init: 1,
        },
        toggleFlagOff: {
            init: 1,
        },
        basicVectorSearch: {
            init: 1,
        },
        vectorSearchOnView: {
            init: 1,
        },
        vectorSearchInUnionWith: {
            init: 1,
        },
        vectorSearchInUnionWithOnView: {
            init: 1,
        },
        dualSearchOuterViewAndInnerView: {
            init: 1,
        },
        vectorSearchOnNestedView: {
            init: 1,
        },
    };

    function setup(db, collName, cluster) {
        // Create the main test collection with vector embeddings
        const coll = db[kTestCollName];
        coll.drop();
        assert.commandWorked(coll.insertMany(kTestData));

        // Create the union target collection (same data, different collection)
        const unionColl = db[kUnionTargetCollName];
        unionColl.drop();
        assert.commandWorked(unionColl.insertMany(kTestData));

        // Create a view with an $addFields transformation.
        db[kViewName].drop();
        assert.commandWorked(db.createView(kViewName, kTestCollName, kViewPipeline));

        // Create a nested view (view on a view).
        db[kNestedViewName].drop();
        assert.commandWorked(db.createView(kNestedViewName, kViewName, kNestedViewPipeline));

        // Create vector search indexes on all collections.
        // Note: This requires mongot to be running and configured.
        jsTest.log.info("Creating vector search indexes...");

        createSearchIndex(db[kTestCollName], kVectorSearchIndexSpec);
        createSearchIndex(db[kUnionTargetCollName], kVectorSearchIndexSpec);

        createSearchIndex(db[kViewName], {...kVectorSearchIndexSpec, name: "view_index"});
        createSearchIndex(db[kNestedViewName], {...kVectorSearchIndexSpec, name: "nested_view_index"});

        jsTest.log.info("Setup complete.");
    }

    function teardown(db, collName, cluster) {
        jsTest.log.info("Teardown: Ensuring feature flag is reset to default state (on)");
        try {
            setParameterOnAllNonConfigNodes(db.getMongo(), "featureFlagVectorSearchExtension", true);
        } catch (e) {
            jsTest.log.warning(`Could not reset feature flag in teardown: ${e.message}`);
        }

        jsTest.log.info("Teardown complete.");
    }

    return {
        threadCount: 4,
        iterations: 50,
        startState: "init",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
