/**
 * This test creates a very basic $addFields view and creates a $search index on it and runs:
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
import {aggPlanHasStage} from "jstests/libs/analyze_plan.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

let bulk = coll.initializeUnorderedBulkOp();
bulk.insert({_id: "New York", state: "NY", pop: 7});
bulk.insert({_id: "Oakland", state: "CA", pop: 6});
bulk.insert({_id: "Palo Alto", state: "CA"});
bulk.insert({_id: "Kansas City", state: "KS"});
bulk.insert({_id: "St Louis", state: "MO"});
bulk.insert({_id: "San Francisco", state: "CA", pop: 4});
bulk.insert({_id: "Trenton", state: "NJ", pop: 5});

assert.commandWorked(bulk.execute());

let viewName = "addFields";
let viewPipeline = [{"$addFields": {pop: {$ifNull: ['$pop', "unknown"]}}}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
let addFieldsView = testDb[viewName];

assert.commandWorked(addFieldsView.createSearchIndex(
    {name: "addFieldsIndex", definition: {"mappings": {"dynamic": true}}}));

// Test basic $search pipeline on the newly created view.
let pipeline = [{
    $search: {
        index: "addFieldsIndex",
        exists: {
            path: "state",
        }
    }
}];
let explain = assert.commandWorked(addFieldsView.explain().aggregate(pipeline)).stages;
// Make sure view transform is last stage of the pipeline.
assertViewAppliedCorrectly(explain, pipeline, viewPipeline);

let results = addFieldsView.aggregate(pipeline).toArray();
let expectedResults = [
    {"_id": "New York", "state": "NY", "pop": 7},
    {"_id": "Trenton", "state": "NJ", "pop": 5},
    {"_id": "Oakland", "state": "CA", "pop": 6},
    {"_id": "Palo Alto", "state": "CA", "pop": "unknown"},
    {"_id": "San Francisco", "state": "CA", "pop": 4},
    {"_id": "Kansas City", "state": "KS", "pop": "unknown"},
    {"_id": "St Louis", "state": "MO", "pop": "unknown"},
];

assertArrayEq({actual: results, expected: expectedResults});

// Test an aggregation pipeline that contains stages after the $search stage.
pipeline = [
    {
        $search: {
            index: "addFieldsIndex",
            exists: {
                path: "state",
            }
        }
    },
    {$project: {pop: 1, _id: 1}}
];
explain = assert.commandWorked(addFieldsView.explain().aggregate(pipeline)).stages;
// Ensure view pipeline is directly after $search stages and before the rest of the user pipeline.
assertViewAppliedCorrectly(explain, pipeline, viewPipeline);

// Test a non-search query on a view indexed by mongot and ensure view transforms are correctly
// applied.
pipeline = [{$match: {pop: {$gt: 4}}}];
let explainResults =
    assert.commandWorked(addFieldsView.explain().aggregate(pipeline)).command.pipeline;

// This utility function will validate that the user pipeline was appended to end of the view
// pipeline.
assertViewAppliedCorrectly(explainResults, pipeline, viewPipeline);

expectedResults = [
    {"_id": "New York", "state": "NY", "pop": 7},
    {"_id": "Trenton", "state": "NJ", "pop": 5},
    {"_id": "Oakland", "state": "CA", "pop": 6},
];
results = addFieldsView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});
