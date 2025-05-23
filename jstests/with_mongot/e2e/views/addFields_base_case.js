/**
 * This test creates a very basic $addFields view, creates a $search index on it, and runs:
 * 1. A basic $search query on the view.
 * 2. An aggregation pipeline containing a $search and non-$search stage.
 * 3. A non-search aggregation pipeline.
 *
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
bulk.insert({_id: "New York", state: "NY", pop: 7});
bulk.insert({_id: "Oakland", state: "CA", pop: 6});
bulk.insert({_id: "Palo Alto", state: "CA"});
bulk.insert({_id: "Kansas City", state: "KS"});
bulk.insert({_id: "St Louis", state: "MO"});
bulk.insert({_id: "San Francisco", state: "CA", pop: 4});
bulk.insert({_id: "Trenton", state: "NJ", pop: 5});
assert.commandWorked(bulk.execute());

const viewName = "addFields";
const viewPipeline = [{"$addFields": {pop: {$ifNull: ['$pop', "unknown"]}}}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
const addFieldsView = testDb[viewName];

const indexConfig = {
    coll: addFieldsView,
    definition: {name: "default", definition: {"mappings": {"dynamic": true}}}
};

const addFieldsBaseCaseTestCases = (isStoredSource) => {
    // =========================================================================================
    // Case 1: Basic $search pipeline on the newly created view.
    // =========================================================================================
    const basicPipeline = [{
        $search: {
            index: "default",
            exists: {
                path: "state",
            },
            returnStoredSource: isStoredSource
        }
    }];

    const basicPipelineExpectedResults = [
        {"_id": "New York", "state": "NY", "pop": 7},
        {"_id": "Trenton", "state": "NJ", "pop": 5},
        {"_id": "Oakland", "state": "CA", "pop": 6},
        {"_id": "Palo Alto", "state": "CA", "pop": "unknown"},
        {"_id": "San Francisco", "state": "CA", "pop": 4},
        {"_id": "Kansas City", "state": "KS", "pop": "unknown"},
        {"_id": "St Louis", "state": "MO", "pop": "unknown"},
    ];

    validateSearchExplain(addFieldsView, basicPipeline, isStoredSource, viewPipeline);

    let results = addFieldsView.aggregate(basicPipeline).toArray();
    assertArrayEq({actual: results, expected: basicPipelineExpectedResults});

    // =========================================================================================
    // Case 2: Aggregation pipeline that contains stages after the $search stage.
    // =========================================================================================
    const pipelineWithStageAfterSearch = [
        {
            $search: {
                index: "default",
                exists: {
                    path: "state",
                },
                returnStoredSource: isStoredSource
            }
        },
        {$project: {pop: 1, _id: 1}}
    ];
    validateSearchExplain(
        addFieldsView, pipelineWithStageAfterSearch, isStoredSource, viewPipeline);

    // =========================================================================================
    // Case 3: Non-search query on a view indexed by mongot, ensuring view transforms are still
    // applied.
    // =========================================================================================
    const nonSearchPipeline = [{$match: {pop: {$gt: 4}}}];

    const nonSearchPipelineExpectedResults = [
        {"_id": "New York", "state": "NY", "pop": 7},
        {"_id": "Trenton", "state": "NJ", "pop": 5},
        {"_id": "Oakland", "state": "CA", "pop": 6},
    ];
    validateSearchExplain(addFieldsView, nonSearchPipeline, isStoredSource, viewPipeline);

    results = addFieldsView.aggregate(nonSearchPipeline).toArray();
    assertArrayEq({actual: results, expected: nonSearchPipelineExpectedResults});
};

createSearchIndexesAndExecuteTests(indexConfig, addFieldsBaseCaseTestCases);
