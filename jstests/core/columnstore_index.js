/**
 * Tests some basic use cases and functionality of a columnstore index.
 * @tags: [
 *   # Uses $indexStats which is not supported inside a transaction.
 *   does_not_support_transactions,
 *
 *   # The test relies on '$indexStats' seeing an earlier read
 *   # operation. So both the $indexStats and the read operation need to be
 *   # sent to the same node.
 *   does_not_support_repeated_reads,
 *   does_not_support_stepdowns,
 *   assumes_read_preference_unchanged,
 *
 *   # column store indexes are still under a feature flag and require full sbe
 *   uses_column_store_index,
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.columnstore_index;
coll.drop();

const csIdx = {
    "$**": "columnstore"
};
assert.commandWorked(coll.createIndex(csIdx));

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
assert.commandWorked(coll.createIndex(csIdx));

let getCSIUsageCount = function(collection) {
    const csi =
        collection
            .aggregate([{$indexStats: {}}, {$match: {$expr: {$eq: ["$key", {$literal: csIdx}]}}}])
            .toArray();
    assert.eq(csi.length, 1);
    return csi[0].accesses.ops;
};

// Test index stats have sensible usage count values for column store indexes.
var usageCount = 0;
assert.eq(getCSIUsageCount(coll), usageCount);
const res = coll.aggregate([{"$project": {"a": 1, _id: 0}}, {"$match": {a: 1}}]);
assert.eq(res.itcount(), 1);
usageCount++;
assert.eq(getCSIUsageCount(coll), usageCount);

// Test collStats have sensible values for column store indexes
const cs = coll.aggregate([{$collStats: {storageStats: {}}}]).next();
assert.eq(cs.storageStats.indexDetails["$**_columnstore"].type, "file");
assert.gt(cs.storageStats.indexSizes["$**_columnstore"], 0);

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
