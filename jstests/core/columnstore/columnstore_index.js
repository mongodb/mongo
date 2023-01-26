/**
 * Tests some basic use cases and functionality of a columnstore index.
 * @tags: [
 *   requires_fcv_63,
 *
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

load("jstests/libs/analyze_plan.js");
load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

if (!setUpServerForColumnStoreIndexTest(db)) {
    return;
}

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
    return csi.reduce((accum, s) => {
        return accum + s.accesses.ops;
    }, 0);
};

// Test index stats have sensible usage count values for column store indexes.
var usageCount = 0;
assert.eq(getCSIUsageCount(coll), usageCount);
const res = coll.aggregate([{"$project": {"a": 1, _id: 0}}, {"$match": {a: 1}}]);
assert.eq(res.itcount(), 1);
usageCount++;
// on sharded collections, each matching shard will reply "1", meaning
// we could get a correct usage total greater than 1.
assert.gte(getCSIUsageCount(coll), usageCount);

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

// Verify that column store indexes appear in listIndexes output.
assert.commandWorked(coll.createIndex({"x.$**": "columnstore"}));
function isIndexSpecInIndexList(expectedIndexSpec, indexList) {
    // The expected specification should appear in the listIndexes output exactly once.
    return indexList.filter(indexSpec => !bsonUnorderedFieldsCompare(indexSpec, expectedIndexSpec))
               .length === 1;
}
let listIndexesResult =
    assert.commandWorked(db.runCommand({listIndexes: coll.getName()})).cursor.firstBatch;
assert(isIndexSpecInIndexList({v: 2, key: {"$**": "columnstore"}, name: "$**_columnstore"},
                              listIndexesResult),
       listIndexesResult);
assert(isIndexSpecInIndexList({v: 2, key: {"x.$**": "columnstore"}, name: "x.$**_columnstore"},
                              listIndexesResult),
       listIndexesResult);

// Test dropping column store indexes.
assert.commandWorked(coll.dropIndex({"$**": "columnstore"}));
assert.commandWorked(coll.dropIndex({"x.$**": "columnstore"}));

// All column store indexes should now be gone.
listIndexesResult =
    assert.commandWorked(db.runCommand({listIndexes: coll.getName()})).cursor.firstBatch;
assert(!tojson(listIndexesResult).includes("columnstore"), listIndexesResult);

// Test that listIndexes shows the columnstoreProjection in indexes that have one.
assert.commandWorked(coll.createIndex({"$**": "columnstore"}, {columnstoreProjection: {x: 1}}));
listIndexesResult =
    assert.commandWorked(db.runCommand({listIndexes: coll.getName()})).cursor.firstBatch;
assert(
    isIndexSpecInIndexList(
        {v: 2, key: {"$**": "columnstore"}, name: "$**_columnstore", columnstoreProjection: {x: 1}},
        listIndexesResult),
    listIndexesResult);
}());
