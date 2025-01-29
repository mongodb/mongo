/**
 * This test issues the search index management commands (createSearchIndex, updateSearchIndex,
 * dropSearchIndex) on sharded views. This test does not use the search library functions as those
 * run $listSearchIndexes, which will not be supported until SERVER-99786.
 *
 * TODO SERVER-99786 : remove this test and just use search_index_commands.js once
 * $listSearchIndexes can support sharded views
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
assertDropCollection(testDb, coll.getName());

// Have to populate collection in order for data to come through the changeStream for mongot to
// replicate (and thus the collection to exist on mongot and be index-able).
assert.commandWorked(coll.insertMany([
    {state: "NY", pop: 19000000, facts: {state_motto: "Excelsior", state_flower: "Rose"}},
    {state: "CA", pop: 39000000, facts: {state_motto: "Eureka", state_flower: "California Poppy"}},
    {
        state: "NJ",
        pop: 9000000,
        facts: {state_motto: "Liberty and Prosperity", state_flower: "Common Blue Violet"}
    },
    {
        state: "AK",
        pop: 3000000,
        facts: {state_motto: "Regnat Populus", state_flower: "Forget-Me-Not"}
    },
]));

let viewName = "addFields";
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', [
    {"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}
]));
let addFieldsView = testDb[viewName];
let searchIndexName = "addFieldsIndex";
let searchIndexCommandResult = assert.commandWorked(testDb.runCommand({
    'createSearchIndexes': viewName,
    'indexes': [{'name': searchIndexName, 'definition': {'mappings': {'dynamic': true}}}]
}));
assert.eq(searchIndexCommandResult.indexesCreated.length, 1);

let indexDef = {
    mappings: {dynamic: true, fields: {}},
    storedSource: {exclude: ["facts.state_motto"]}
};

// update and drop just give ok messages, there is no array of indexes changed that you can assert
// on.
searchIndexCommandResult = assert.commandWorked(testDb.runCommand(
    {'updateSearchIndex': viewName, name: searchIndexName, definition: indexDef}));

searchIndexCommandResult =
    assert.commandWorked(testDb.runCommand({'dropSearchIndex': viewName, name: searchIndexName}));
