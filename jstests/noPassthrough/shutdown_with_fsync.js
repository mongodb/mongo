/**
 * Tests that shutdown can succeed even if the server is fsync locked.
 */

(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(conn, null);

const dbName = jsTestName();
const collName = "testColl";
const testDB = conn.getDB(dbName);
const testColl = testDB.getCollection(collName);

jsTestLog("Insert some data to create a collection.");
assert.commandWorked(testColl.insert({x: 1}));

jsTestLog("Set fsync lock to block server writes. Create some nesting for extra test coverage");
testDB.fsyncLock();
testDB.fsyncLock();

jsTestLog("Check that the fsync lock is working: no writes should be possible.");
assert.commandFailed(testDB.runCommand({insert: {z: 1}, maxTimeMS: 30}));

jsTestLog("Check that shutdown can succeed with an fsync lock: the fsync lock should be cleared.");
// Skipping validation because the fsync lock causes the validate command to hang.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
}());
