/**
 * Test that the size storer runs periodically in the event of a server crash, not just on clean
 * shutdown.
 *
 * This test requires persistence to ensure data survives a restart.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

// Set up the data files to be reused across server restarts.
const dbpath = MongoRunner.dataPath + jsTestName();
resetDbpath(dbpath);
const mongodArgs = {
    dbpath: dbpath,
    noCleanData: true
};

let conn = MongoRunner.runMongod(mongodArgs);
let testDB = conn.getDB(jsTestName());
let testColl = testDB.test;

// Set up the collection with some data. The fsync command will flush the size storer.
assert.commandWorked(testColl.insert({y: "insertedDataInitialize"}));
assert.commandWorked(testDB.adminCommand({fsync: 1}));

// First test that fast count data is lost. The size storer flushes every 100,000 operations or 60
// seconds. 10 documents should not take long, so the 60 seconds flush should not trigger.
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(testColl.insert({x: i}));
}
assert.eq(11, testColl.count());

MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
conn = MongoRunner.runMongod(mongodArgs);
assert.neq(conn, null, 'mongod was unable to restart after receiving a SIGKILL');
testDB = conn.getDB(jsTestName());
testColl = testDB.test;
jsTestLog("Recovery after first crash. Fast count: " + testColl.count() +
          ", number of docs: " + tojson(testColl.find({}).toArray().length));

assert.eq(1,
          testColl.count(),
          "Fast count should be incorrect after a server crash. Fast count: " + testColl.count());

// Second, ensure that fast count data saved after 60 seconds is present after a server crash.
for (let i = 0; i < 100; ++i) {
    assert.commandWorked(testColl.insert({x: i}));
}
assert.eq(testColl.count(), 101, "Fast count should be 100 + 1. Fast count: " + testColl.count());

jsTestLog("Sleep > 60 seconds to wait for the size storer to be ready to flush.");
sleep(65 * 1000);
jsTestLog("Awake. Doing one more write to trigger a flush, if some internal op didn't already.");
// The fast count should definitely be at least 101, but the fast count update to 102 for the
// {y: "triggeringSizeStorerFlush"} write may or may not be persisted, depending on whether the
// write triggered the flush or some internal write already has and reset the 60 second timer.
assert.commandWorked(testColl.insert({y: "triggeringSizeStorerFlush"}));

MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
conn = MongoRunner.runMongod(mongodArgs);
assert.neq(conn, null, 'mongod was unable to restart after receiving a SIGKILL');
testDB = conn.getDB(jsTestName());
testColl = testDB.test;
jsTestLog("Recovery after second crash. Fast count: " + testColl.count() +
          ", number of docs: " + tojson(testColl.find({}).toArray().length));

assert.gte(testColl.count(),
           101,
           "Fast count should still be 100 + 1 after crash. Fast count: " + testColl.count());

MongoRunner.stopMongod(conn);
}());
