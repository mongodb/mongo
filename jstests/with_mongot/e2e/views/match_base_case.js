/**
 * This test creates a very basic $match view and creates a $search index on it and runs:
 * 1. basic $search query on the view
 * 2. aggregation pipeline containing a $search and non-$search stage
 * 3. non-search aggregation pipeline
 *
 * Each test case includes running an explain to ensure the user and view stages are in the correct
 * order.
 *
 * @tags: [
 * requires_mongot_1_40
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

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

let viewName = "match";
let viewPipeline =
    [{"$match": {"$expr": {"$gt": [{"$subtract": ["$stock", "$num_orders"]}, 300]}}}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
let matchView = testDb[viewName];

assert.commandWorked(
    matchView.createSearchIndex({name: "matchIndex", definition: {"mappings": {"dynamic": true}}}));
let userPipeline = [{
    $search: {
        index: "matchIndex",
        exists: {
            path: "_id",
        }
    }
}];
let explainResults = assert.commandWorked(matchView.explain().aggregate(userPipeline)).stages;
assertViewAppliedCorrectly(explainResults, userPipeline, viewPipeline);

let results = matchView.aggregate(userPipeline).toArray();

let expectedResults = [
    {"_id": "android", "stock": 500, "num_orders": 99},
    {"_id": "charger", "stock": 456, "num_orders": 100},
    {"_id": "keyboard", "stock": 928, "num_orders": 102}
];

assertArrayEq({actual: results, expected: expectedResults});

// Test an aggregation pipeline that contains stages after the $search stage.
userPipeline = [
    {
        $search: {
            index: "matchIndex",
            exists: {
                path: "_id",
            }
        }
    },
    {$project: {_id: 1}}
];
explainResults = matchView.explain().aggregate(userPipeline)["stages"];
// Ensure view pipeline is directly after $search stages and before the rest of the user pipeline.
assertViewAppliedCorrectly(explainResults, userPipeline, viewPipeline);

// Test a non-search query on a view indexed by mongot and ensure view transforms are correctly
// applied.
userPipeline = [{$project: {_id: 1}}];
explainResults = matchView.explain().aggregate(userPipeline)["command"]["pipeline"];
// This utility function will validate that the user pipeline was appended to end of the view
// pipeline eg view pipeline then user pipeline.
assertViewAppliedCorrectly(explainResults, userPipeline, viewPipeline);
