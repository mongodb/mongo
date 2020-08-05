/**
 * Tests readConcern level snapshot outside of transactions.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const collName = "coll";
const primaryDB = replSet.getPrimary().getDB('test');
const collection = primaryDB[collName];

const docs = [...Array(10).keys()].map((i) => ({"_id": i}));
const insertTimestamp =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: docs})).operationTime;
jsTestLog("Inserted 10 documents at: " + tojson(insertTimestamp));

// Test find with atClusterTime.
let cursor = collection.find().readConcern("snapshot", insertTimestamp);
assert.eq(cursor.getClusterTime(), insertTimestamp);

cursor = collection.find().readConcern("snapshot", insertTimestamp).batchSize(2);
cursor.next();
cursor.next();
assert.eq(cursor.objsLeftInBatch(), 0);
// This triggers a getMore.
cursor.next();
// Test that the read timestamp remains the same after the getMore.
assert.eq(cursor.getClusterTime(), insertTimestamp);

// Test find with snapshot readConcern.
cursor = collection.find().readConcern("snapshot");
assert.eq(cursor.getClusterTime(), insertTimestamp);

// Test find with non-snapshot readConcern.
cursor = collection.find();
assert.eq(cursor.getClusterTime(), undefined);

// Test aggregate with atClusterTime.
cursor = collection.aggregate([{$sort: {_id: 1}}],
                              {readConcern: {level: "snapshot", atClusterTime: insertTimestamp}});
assert.eq(cursor.getClusterTime(), insertTimestamp);

// Test aggregate with snapshot readConcern.
cursor = collection.aggregate([{$sort: {_id: 1}}], {readConcern: {level: "snapshot"}});
assert.eq(cursor.getClusterTime(), insertTimestamp);

// Test aggregate with non-snapshot readConcern.
cursor = collection.aggregate([{$sort: {_id: 1}}]);
assert.eq(cursor.getClusterTime(), undefined);

replSet.stopSet();
})();
