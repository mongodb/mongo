/**
 * This test creates a very basic $match view with a $search index on it and runs:
 * 1. A basic $search query on the view.
 * 2. An aggregation pipeline containing a $search and non-$search stage.
 * 3. A non-search aggregation pipeline.
 *
 * Each test case includes running an explain to ensure the user and view stages are in the correct
 * order.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

let bulk = coll.initializeUnorderedBulkOp();
bulk.insert({_id: "iPhone", stock: 1000, num_orders: 709});
bulk.insert({_id: "android", stock: 500, num_orders: 99});
bulk.insert({_id: "charger", stock: 456, num_orders: 100});
bulk.insert({_id: "keyboard", stock: 928, num_orders: 102});
bulk.insert({_id: "mouse", stock: 300, num_orders: 203});
bulk.insert({_id: "pen", stock: 600, num_orders: 403});
bulk.insert({_id: "monitor", stock: 880, num_orders: 581});
assert.commandWorked(bulk.execute());

const viewName = "match";
const viewPipeline =
    [{"$match": {"$expr": {"$gt": [{"$subtract": ["$stock", "$num_orders"]}, 300]}}}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
const matchView = testDb[viewName];

const indexConfig = {
    coll: matchView,
    definition: {name: "default", definition: {"mappings": {"dynamic": true}}}
};

const matchBaseCaseTestCases = (isStoredSource) => {
    // ===============================================================================
    // Case 1: Basic $search pipeline.
    // ===============================================================================
    const searchPipeline = [{
        $search: {
            index: "default",
            exists: {
                path: "_id",
            },
            returnStoredSource: isStoredSource
        }
    }];

    const expectedResults = [
        {"_id": "android", "stock": 500, "num_orders": 99},
        {"_id": "charger", "stock": 456, "num_orders": 100},
        {"_id": "keyboard", "stock": 928, "num_orders": 102}
    ];

    validateSearchExplain(matchView, searchPipeline, isStoredSource, viewPipeline);

    let results = matchView.aggregate(searchPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===============================================================================
    // Case 2: Aggregation pipeline with $search and projection.
    // ===============================================================================
    const projectionPipeline = [
        {
            $search: {
                index: "default",
                exists: {
                    path: "_id",
                },
                returnStoredSource: isStoredSource
            }
        },
        {$project: {_id: 1}}
    ];

    validateSearchExplain(matchView, projectionPipeline, isStoredSource, viewPipeline);

    // ===============================================================================
    // Case 3: Non-search query to ensure view transforms are applied.
    // ===============================================================================
    const nonSearchPipeline = [{$project: {_id: 1}}];

    validateSearchExplain(matchView, nonSearchPipeline, isStoredSource);
};

createSearchIndexesAndExecuteTests(indexConfig, matchBaseCaseTestCases);
