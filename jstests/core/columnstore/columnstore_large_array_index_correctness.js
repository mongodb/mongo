/**
 * Testing of just the query layer's integration for columnar indexes that encode large arrays.
 * @tags: [
 *   # Column store indexes are still under a feature flag.
 *   featureFlagColumnstoreIndexes,
 *   # Columnstore tests set server parameters to disable columnstore query planning heuristics -
 *   # 1) server parameters are stored in-memory only so are not transferred onto the recipient,
 *   # 2) server parameters may not be set in stepdown passthroughs because it is a command that may
 *   #      return different values after a failover
 *   tenant_migration_incompatible,
 *   does_not_support_stepdowns,
 *   not_allowed_with_security_token,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");      // For "planHasStage."
load("jstests/libs/columnstore_util.js");  // For "setUpServerForColumnStoreIndexTest."

if (!setUpServerForColumnStoreIndexTest(db)) {
    return;
}

const coll = db.columnstore_large_array_index_correctness;
coll.drop();

const uint8 = {
    _num: 0,
    o: Array.from({length: 50}, (_, i) => ({b: [2 * i, 2 * i + 1]})),
};
const uint16 = {
    _num: 1,
    o: Array.from({length: 150}, (_, i) => ({b: [2 * i, 2 * i + 1]})),
};
const uint32 = {
    _num: 2,
    o: Array.from({length: 15000}, (_, i) => ({b: [2 * i, 2 * i + 1]})),
};

const docs = [uint8, uint16, uint32];

for (let doc of docs) {
    coll.insert(doc);
}

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));
const kProjection = {
    _id: 0,
    _num: 1,
    "o.b": 1,
};

// Ensure this test is exercising the column scan.
let explain = coll.find({}, kProjection).sort({_num: 1}).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"), explain);

// Run a query getting all of the results using the column index.
let results = coll.find({}, kProjection).sort({_num: 1}).toArray();
assert.gt(results.length, 0);

// Run a query getting all results without column index
let trueResults = coll.find({}, kProjection).hint({$natural: 1}).sort({_num: 1}).toArray();

assert.eq(results.length, trueResults.length);

for (let i = 0; i < results.length; i++) {
    const originalDoc = coll.findOne({_num: results[i]._num});
    assert.eq(results[i], trueResults[i], () =>
            `column store index output number: ${results[i]._num}, collection scan output number: ${trueResults[i]._num},
             original document number was: ${originalDoc._num}`);
}
})();
