/**
 * This test uses nested view to refer to a view that is created on top of another view. This test
 * validates that mongod correctly resolves the underlying namespace of the nested view in its
 * request to mongot by asserting the results of the $search query.
 *
 * This test does not use the search library functions as those
 * run $listSearchIndexes, which will not be supported until SERVER-99786.
 *
 * TODO SERVER-99786 : remove this test and just use nested_view.js once $listSearchIndexes can
 * support sharded views
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
assertDropCollection(testDb, coll.getName());

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
let nestedView = testDb["nestedView"];

let searchIndexName = "foo";
let searchIndexCommandResult = assert.commandWorked(testDb.runCommand({
    'createSearchIndexes': nestedView.getName(),
    'indexes': [{'name': searchIndexName, 'definition': {'mappings': {'dynamic': true}}}]
}));
assert.eq(searchIndexCommandResult.indexesCreated.length, 1);

let indexDef = {mappings: {dynamic: true, fields: {}}, storedSource: {exclude: ["pop"]}};

// update and drop just give ok messages, there is no array of indexes changed that you can assert
// on.
searchIndexCommandResult = assert.commandWorked(testDb.runCommand(
    {'updateSearchIndex': nestedView.getName(), name: searchIndexName, definition: indexDef}));

searchIndexCommandResult = assert.commandWorked(
    testDb.runCommand({'dropSearchIndex': nestedView.getName(), name: searchIndexName}));
