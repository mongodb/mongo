/**
 * Tests some basic use cases and functionality of a columnstore index.
 * @tags: [
 *   # Uses $indexStats which is not supported inside a transaction.
 *   does_not_support_transactions,
 *   # columnstore indexes are new in 6.2.
 *   requires_fcv_62,
 *   uses_column_store_index,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const columnstoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index validation test since the feature flag is not enabled.");
    return;
}

const coll = db.columnstore_index;
coll.drop();

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

// Test that we can indeed insert and index documents.
assert.commandWorked(coll.insert({_id: 0, x: 1, y: 1}));
// Also should be able to do updates and removes. Target by _id to ensure the test still works in
// sharded collection passthroughs.
assert.commandWorked(coll.replaceOne({_id: 0, x: 1, y: 1}, {x: 1, y: "new"}));
assert.commandWorked(coll.updateOne({_id: 0}, {$unset: {y: 1}}));
assert.commandWorked(coll.updateOne({_id: 0}, [{$set: {y: "Guess who's back"}}]));
assert.commandWorked(coll.deleteOne({_id: 0}));

// Test inserting multiple things at once
assert.commandWorked(coll.insert([{_id: 1}, {_id: 2}, {_id: 3, x: 1}]));
coll.drop();

// Test building index after there is already data - should enable using a bulk builder.
const allDocs = [{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 1}, {_id: 3, a: 3, b: 1}];
assert.commandWorked(coll.insert(allDocs));
assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

// Test returning index stats. TODO SERVER-65980 assert that we get something sensible
assert.doesNotThrow(() => coll.aggregate([{$indexStats: {}}]));
// Test returning collection stats. TODO SERVER-65980 assert that we get something sensible
assert.doesNotThrow(() => coll.aggregate([{$collStats: {}}]));

// Test running validate.
assert.commandWorked(coll.validate());
assert.commandWorked(coll.validate({full: true}));

// Test running a query with the index.
let explain = coll.find({}, {a: 1, b: 1}).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"));
assert.sameMembers(coll.find({}, {a: 1, b: 1}).toArray(), allDocs);

// Test dropping the index.
assert.commandWorked(coll.dropIndex({"$**": "columnstore"}));
}());
