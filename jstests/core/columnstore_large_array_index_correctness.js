/**
 * Testing of just the query layer's integration for columnar indexes that encode large arrays.
 * @tags: [
 *   # column store indexes are still under a feature flag and require full sbe
 *   uses_column_store_index,
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For "planHasStage."

const coll = db.columnstore_large_array_index_correctness;
coll.drop();

const uint8 = {
    num: 0,
    a: Array.from({length: 50}, (_, i) => ({b: [2 * i, 2 * i + 1]}))
};
const uint16 = {
    num: 1,
    a: Array.from({length: 150}, (_, i) => ({b: [2 * i, 2 * i + 1]}))
};
const uint32 = {
    num: 2,
    a: Array.from({length: 15000}, (_, i) => ({b: [2 * i, 2 * i + 1]}))
};

const docs = [uint8, uint16, uint32];

for (let doc of docs) {
    coll.insert(doc);
}

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));
const kProjection = {
    _id: 0,
    "a.b": 1,
    num: 1,
};

// Ensure this test is exercising the column scan.
let explain = coll.find({}, kProjection).sort({num: 1}).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);

// Run a query getting all of the results using the column index.
let results = coll.find({}, kProjection).sort({num: 1}).toArray();
assert.gt(results.length, 0);

// Run a query getting all results without column index
let trueResults = coll.find({}, kProjection).hint({$natural: 1}).sort({num: 1}).toArray();

assert.eq(results.length, trueResults.length);

for (let i = 0; i < results.length; i++) {
    const originalDoc = coll.findOne({num: results[i].num});
    assert.eq(results[i], trueResults[i], () =>
            `column store index output number: ${results[i].num}, collection scan output number: ${trueResults[i].num},
             original document number was: ${originalDoc.num}`);
}
})();
