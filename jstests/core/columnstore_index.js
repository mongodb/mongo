/**
 * Tests some basic use cases and functionality of a columnstore index.
 * @tags: [
 *   # Uses $indexStats which is not supported inside a transaction.
 *   does_not_support_transactions,
 *   # columnstore indexes are new in 6.1.
 *   requires_fcv_61,
 *   # Columnstore indexes are incompatible with clustered collections.
 *   incompatible_with_clustered_collection,
 *   # We could potentially need to resume an index build in the event of a stepdown, which is not
 *   # yet implemented.
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

const getParamResponse =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagColumnstoreIndexes: 1}));
const columnstoreEnabled = getParamResponse.hasOwnProperty("featureFlagColumnstoreIndexes") &&
    getParamResponse.featureFlagColumnstoreIndexes.value;
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
assert.commandWorked(coll.insert([{_id: 1}, {_id: 2}, {_id: 3, x: 1}]));
assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

// Test returning index stats. TODO SERVER-65980 assert that we get something sensible
assert.doesNotThrow(() => coll.aggregate([{$indexStats: {}}]));
// Test returning collection stats. TODO SERVER-65980 assert that we get something sensible
assert.doesNotThrow(() => coll.aggregate([{$collStats: {}}]));

// Test running validate.
assert.commandWorked(coll.validate());
assert.commandWorked(coll.validate({full: true}));

// Test dropping the index.
assert.commandWorked(coll.dropIndex({"$**": "columnstore"}));
}());
