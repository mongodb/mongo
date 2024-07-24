/**
 * Tests that a hashed index used by a $lookup will produce the correct results whether or not the
 * field being used in that index is the hashed one (SERVER-92668).
 */
load("jstests/aggregation/extras/utils.js");  // for resultsEq()

const coll = db.lookup_using_hashed_index;
const collName = coll.getName();
const testIndexName = "testIndex";
coll.drop();

const allDocs = [{_id: 0}, {_id: 1, a: 3}, {_id: 2, a: "3"}];
assert.commandWorked(coll.insert(allDocs));

// Each document should look itself up in every test.
const expected = allDocs.map(doc => Object.merge(doc, {relookup: [doc]}));

const pipeline = [{$lookup: {from: collName, localField: "a", foreignField: "a", as: "relookup"}}];

// Test 1: foreignField IS the hashed field.
assert.commandWorked(coll.createIndex({a: "hashed", b: 1}, {name: testIndexName}));
let results = coll.aggregate(pipeline).toArray();
assert(resultsEq(results, expected, true), [results, expected]);
assert.commandWorked(coll.dropIndex(testIndexName));

// Test 2: foreignField is NOT the hashed field.
assert.commandWorked(coll.createIndex({a: 1, b: "hashed"}, {name: testIndexName}));
results = coll.aggregate(pipeline).toArray();
assert(resultsEq(results, expected, true), [results, expected]);
assert.commandWorked(coll.dropIndex(testIndexName));
