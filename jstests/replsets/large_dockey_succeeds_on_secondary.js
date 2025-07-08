/**
 * Tests that a document with a large dockey (_id + shard key) will succeed during replication to
 * secondaries. There is no size limit on the _id field, so it is possible for a document to have an
 * _id up to the maximum document size (16MB). The oplog entry will duplicate the _id field as a
 * part of the document key, so this test checks that documents with very large _ids can be written
 * and retrieved successfully.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "testdb";
const collName = "testcoll";

// Create a replSetTest with 2 nodes (primary and secondary).
const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

jsTestLog("Insert a small document to ensure the set is running normally.");
assert.commandWorked(primaryColl.insert([{x: 1, y: 2}, {x: 2, y: 2}]));

jsTestLog("Insert a 16MB document with an _id that is the maximum size.");
// Subtract 16 bytes to accommodate the space taken up by the field name.
const idSize = (16 * 1024 * 1024) - 16;
assert.commandWorked(primaryColl.insert({_id: "Z".repeat(idSize)}));

jsTestLog("Await replication of the large document to the secondary.");
rst.awaitReplication();

jsTestLog("Retrieve the inserted documents.");
let result = primaryColl.find().toArray();

jsTestLog("Result:");
jsTestLog(result);

rst.stopSet();
