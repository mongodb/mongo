/**
 * Tests readConcern level snapshot outside of transactions.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const collName = "coll";
const primaryDB = replSet.getPrimary().getDB("test");
const collection = primaryDB[collName];

const docs = [...Array(10).keys()].map((i) => ({"_id": i}));
const insertTimestamp = assert.commandWorked(primaryDB.runCommand({insert: collName, documents: docs})).operationTime;
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
// During primary stepup, we will rebuild PrimaryOnlyService instances, which requires creating
// indexes on certain collections. If a createCollection occurred after the insert of 10 documents,
// it is possible that the committed snapshot advanced past 'insertTimestamp'. Therefore, this read
// could be reading at a newer snapshot since we did not specify a specific 'atClusterTime'.
assert.gte(cursor.getClusterTime(), insertTimestamp);

// Test find with non-snapshot readConcern.
cursor = collection.find();
assert.eq(cursor.getClusterTime(), undefined);

// Test aggregate with atClusterTime.
cursor = collection.aggregate([{$sort: {_id: 1}}], {readConcern: {level: "snapshot", atClusterTime: insertTimestamp}});
assert.eq(cursor.getClusterTime(), insertTimestamp);

// Test aggregate with snapshot readConcern. Similarly to the find with snapshot readConcern and no
// 'atClusterTime', it's possible that this aggregate can read at a newer snapshot than
// 'insertTimestamp'.
cursor = collection.aggregate([{$sort: {_id: 1}}], {readConcern: {level: "snapshot"}});
assert.gte(cursor.getClusterTime(), insertTimestamp);

// Test aggregate with non-snapshot readConcern.
cursor = collection.aggregate([{$sort: {_id: 1}}]);
assert.eq(cursor.getClusterTime(), undefined);

replSet.stopSet();
