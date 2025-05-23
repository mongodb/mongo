/**
 * This test uses a nested view to refer to a view that is created on top of another view. This test
 * validates that mongod correctly resolves the underlying namespace of the nested view in its
 * request to mongot by asserting the results of the $search query.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {updateSearchIndex} from "jstests/libs/search.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

let bulk = coll.initializeUnorderedBulkOp();
bulk.insert({_id: "New York", state: "NY", pop: 7});
bulk.insert({_id: "Oakland", state: "CA", pop: 3});
bulk.insert({_id: "Palo Alto", state: "CA", pop: 10});
bulk.insert({_id: "San Francisco", state: "CA", pop: 4});
bulk.insert({_id: "Trenton", state: "NJ", pop: 5});
assert.commandWorked(bulk.execute());

const viewName = "baseView";
const viewPipeline = [{"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));

const nestedViewPipeline = [{"$addFields": {bb_type: {$ifNull: ['$bb_type', 'foo']}}}];
assert.commandWorked(testDb.createView("nestedView", viewName, nestedViewPipeline));

const nestedView = testDb["nestedView"];
const combinedViewPipeline = viewPipeline.concat(nestedViewPipeline);

const indexConfig = {
    coll: nestedView,
    definition: {name: "foo", definition: {"mappings": {"dynamic": true}}}
};

const nestedViewTestCases = () => {
    let results = nestedView.aggregate({$listSearchIndexes: {}}).toArray();
    assert.eq(results.length, 1);
    assert.eq(results[0].name, "foo");

    // =========================================================================================
    // Case 1: Basic search query on nested view.
    // =========================================================================================
    const basicSearchPipeline = [{
        $search: {
            index: "foo",
            exists: {
                path: "state",
            }
        }
    }];

    const expectedResults = [
        {"_id": "Oakland", "state": "CA", "pop": 3, "aa_type": "foo", "bb_type": "foo"},
        {"_id": "San Francisco", "state": "CA", "pop": 4, "aa_type": "foo", "bb_type": "foo"},
        {"_id": "Trenton", "state": "NJ", "pop": 5, "aa_type": "foo", "bb_type": "foo"},
        {"_id": "Palo Alto", "state": "CA", "pop": 10, "aa_type": "foo", "bb_type": "foo"},
        {"_id": "New York", "state": "NY", "pop": 7, "aa_type": "foo", "bb_type": "foo"}
    ];

    validateSearchExplain(nestedView, basicSearchPipeline, false, combinedViewPipeline);

    results = nestedView.aggregate(basicSearchPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // =========================================================================================
    // Case 2: Update index with storedSource exclusion and run search with returnStoredSource.
    // =========================================================================================
    const indexDef = {mappings: {dynamic: true, fields: {}}, storedSource: {exclude: ["state"]}};
    updateSearchIndex(nestedView, {name: "foo", definition: indexDef});

    const wildcardSearchPipeline = [{
        $search: {
            index: "foo",
            wildcard: {
                query: "*",  // This matches all documents.
                path: "_id",
                allowAnalyzedField: true,
            },
            returnStoredSource: true
        }
    }];

    const expectedStoredSourceResults = [
        {_id: "Palo Alto", pop: 10, aa_type: "foo", bb_type: "foo"},
        {_id: "Oakland", pop: 3, aa_type: "foo", bb_type: "foo"},
        {_id: "Trenton", pop: 5, aa_type: "foo", bb_type: "foo"},
        {_id: "New York", pop: 7, aa_type: "foo", bb_type: "foo"},
        {_id: "San Francisco", pop: 4, aa_type: "foo", bb_type: "foo"}
    ];

    validateSearchExplain(nestedView, wildcardSearchPipeline, true, combinedViewPipeline);

    results = nestedView.aggregate(wildcardSearchPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedStoredSourceResults});
};

createSearchIndexesAndExecuteTests(indexConfig, nestedViewTestCases, false);
