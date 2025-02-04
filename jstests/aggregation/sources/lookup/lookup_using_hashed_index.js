/**
 * Tests that a hashed index used by a $lookup will produce the correct results whether or not the
 * field being used in that index is the hashed one (SERVER-92668).
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db.lookup_using_hashed_index;
coll.drop();

const foreignColl = db.lookup_using_hashed_index_foreign;
const foreignCollName = foreignColl.getName();
foreignColl.drop();

const testIndexName = "testIndex";

// Creates 'indexForeignColl' on foreignColl with 'options', and checks that the result of running
// an aggregation with 'pipeline' with 'options' matches 'expected'.
function runTest(indexForeignColl, pipeline, expected, options = {}) {
    assert.commandWorked(
        foreignColl.createIndex(indexForeignColl, {name: testIndexName, ...options}));
    const results = coll.aggregate(pipeline, options).toArray();
    assert(resultsEq(results, expected, true), [results, expected]);
    assert.commandWorked(foreignColl.dropIndex(testIndexName));
}

const allDocs = [{_id: 0}, {_id: 1, a: 3}, {_id: 2, a: "3"}];
assert.commandWorked(coll.insert(allDocs));
assert.commandWorked(foreignColl.insert(allDocs));

// Each document should look up the equivalent document in the foreign collection since both
// collections are identical.
const expected = allDocs.map(doc => Object.merge(doc, {relookup: [doc]}));

const pipeline =
    [{$lookup: {from: foreignCollName, localField: "a", foreignField: "a", as: "relookup"}}];

// Test 1: foreignField IS the hashed field.
runTest({a: "hashed", b: 1}, pipeline, expected);

// Test 2: foreignField is NOT the hashed field.
runTest({a: 1, b: "hashed"}, pipeline, expected);

assert.commandWorked(coll.insert({_id: 3, str: "strstrstr"}));
assert.commandWorked(foreignColl.insert({_id: 5, str: "STRSTRSTR"}));
assert.commandWorked(foreignColl.insert({_id: 6, str: null}));
const collationOptions = {
    collation: {
        locale: 'ru',
        strength: 1,
    },
};
const collatedPipeline =
    [{$lookup: {from: foreignCollName, localField: "str", foreignField: "str", as: "relookup"}}];
const collatedExpected = [
    {
        _id: 0,
        relookup: [{"_id": 0}, {"_id": 1, "a": 3}, {"_id": 2, "a": "3"}, {"_id": 6, "str": null}]
    },
    {
        _id: 1,
        a: 3,
        relookup: [{"_id": 0}, {"_id": 1, "a": 3}, {"_id": 2, "a": "3"}, {"_id": 6, "str": null}]
    },
    {
        _id: 2,
        a: "3",
        relookup: [{"_id": 0}, {"_id": 1, "a": 3}, {"_id": 2, "a": "3"}, {"_id": 6, "str": null}]
    },
    {_id: 3, str: "strstrstr", relookup: [{_id: 5, str: "STRSTRSTR"}]}
];
// Test 3: collscan.
runTest({nonExistentField: 1}, collatedPipeline, collatedExpected, collationOptions);

// Test 4: foreignField IS the hashed field with collation specified.
runTest({str: "hashed", a: 1}, collatedPipeline, collatedExpected, collationOptions);

// Test 5: foreignField is NOT the hashed field with collation specified.
runTest({str: 1, a: "hashed"}, collatedPipeline, collatedExpected, collationOptions);
