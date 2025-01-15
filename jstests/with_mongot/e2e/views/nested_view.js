/**
 * This test uses nested view to refer to a view that is created on top of another view. This test
 * validates that mongod correctly resolves the underlying namespace of the nested view in its
 * request to mongot by asserting the results of the $search query.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

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

let viewName = "baseView";
let viewPipeline = [{"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
let view = testDb[viewName];
let nestedViewPipeline = [{"$addFields": {bb_type: {$ifNull: ['$bb_type', 'foo']}}}];
assert.commandWorked(testDb.createView("nestedView", viewName, nestedViewPipeline));
// Cannot create index as the view 'addFields' was deleted or its source collection has changed

let nestedView = testDb["nestedView"];
createSearchIndex(nestedView, {name: "foo", definition: {"mappings": {"dynamic": true}}});
let pipeline = [{
    $search: {
        index: "foo",
        exists: {
            path: "state",
        }
    }
}];
let explainResults = assert.commandWorked(nestedView.explain().aggregate(pipeline)).stages;
// Ensure the explain results contain the outer view stages and nested view stages.
assertViewAppliedCorrectly(explainResults, pipeline, viewPipeline.concat(nestedViewPipeline));

let results = nestedView.aggregate(pipeline).toArray();

let expectedResults = [
    {"_id": "Oakland", "state": "CA", "pop": 3, "aa_type": "foo", "bb_type": "foo"},
    {"_id": "San Francisco", "state": "CA", "pop": 4, "aa_type": "foo", "bb_type": "foo"},
    {"_id": "Trenton", "state": "NJ", "pop": 5, "aa_type": "foo", "bb_type": "foo"},
    {"_id": "Palo Alto", "state": "CA", "pop": 10, "aa_type": "foo", "bb_type": "foo"},
    {"_id": "New York", "state": "NY", "pop": 7, "aa_type": "foo", "bb_type": "foo"}
];

assertArrayEq({actual: results, expected: expectedResults});
dropSearchIndex(nestedView, {name: "foo"});