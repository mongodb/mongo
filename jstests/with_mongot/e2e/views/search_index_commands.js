/**
 * This test issues the search index management commands (createSearchIndex, updateSearchIndex) on
 * views.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex, updateSearchIndex} from "jstests/libs/search.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

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

let indexDef = {mappings: {dynamic: true}, storedSource: {exclude: ["facts.state_motto"]}};
createSearchIndex(addFieldsView, {name: "addFieldsIndex", definition: indexDef});

// Update the search index to exclude state_flower, instead of state_motto, from storage.
indexDef.storedSource = {
    exclude: ["facts.state_flower"]
};

updateSearchIndex(addFieldsView, {name: "addFieldsIndex", definition: indexDef});

let results = addFieldsView.aggregate([{$listSearchIndexes: {name: "addFieldsIndex"}}]).toArray();
assert(results.length == 1);
// Make sure the index has the updated index definition.
assert.eq(results[0].latestDefinition.storedSource, indexDef.storedSource);

dropSearchIndex(addFieldsView, {name: "addFieldsIndex"});
