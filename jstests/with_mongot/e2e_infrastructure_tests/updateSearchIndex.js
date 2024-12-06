/**
 * This test validates that updateSearchIndex works for standalone and sharded configurations.
 */
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

let indexDef = {mappings: {dynamic: true}, storedSource: {exclude: ["facts.state_motto"]}};
createSearchIndex(coll, {name: "updateSearchIndexTest", definition: indexDef});

// This query returns all documents in the collection but it is kind of silly as a search query.
let pipeline = [{
    $search: {
        index: "updateSearchIndexTest",
        wildcard: {
            query: "*",  // This matches all documents
            path: "state",
            allowAnalyzedField: true,
        },
        returnStoredSource: true
    }
}];
let results = coll.aggregate(pipeline).toArray();
// Since this is a returnStoredSource query, the results should only include the fields that mongot
// is storing as dictated by our search index definition.
results.forEach(state => {
    assert(!state.facts.hasOwnProperty("state_motto"));
    assert(state.facts.hasOwnProperty("state_flower"));
});

// Update the search index to exclude state_flower, instead of state_motto, from storage.
indexDef.storedSource = {
    exclude: ["facts.state_flower"]
};
updateSearchIndex(coll, {name: "updateSearchIndexTest", definition: indexDef});

results = coll.aggregate(pipeline).toArray();
results.forEach(state => {
    assert(!state.facts.hasOwnProperty("state_flower"));
    assert(state.facts.hasOwnProperty("state_motto"));
});

dropSearchIndex(coll, {name: "updateSearchIndexTest"});