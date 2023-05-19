/*
 * This test ensures an index build can yield during bulk load phase.
 */
(function() {

"use strict";

load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/fail_point_util.js");

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);

TestData.dbName = jsTestName();
TestData.collName = "coll";

const testDB = conn.getDB(TestData.dbName);
testDB.getCollection(TestData.collName).drop();

assert.commandWorked(testDB.createCollection(TestData.collName));
const coll = testDB.getCollection(TestData.collName);

for (let i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({_id: i, x: i}));
}

// Make the index build bulk load yield often.
assert.commandWorked(
    conn.adminCommand({setParameter: 1, internalIndexBuildBulkLoadYieldIterations: 1}));

jsTestLog("Enable hangDuringIndexBuildBulkLoadYield fail point");
let failpoint = configureFailPoint(
    testDB, "hangDuringIndexBuildBulkLoadYield", {namespace: coll.getFullName()});

jsTestLog("Create index");
const awaitIndex = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), coll.getFullName(), {x: 1}, {}, [ErrorCodes.IndexBuildAborted]);

// Wait until index build (bulk load phase) yields.
jsTestLog("Wait for the index build to yield and hang");
failpoint.wait();

jsTestLog("Drop the collection");
const awaitDrop = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({drop: TestData.collName}));
}, conn.port);

// Wait until the index build starts aborting to make sure the drop happens before the index build
// finishes.
checkLog.containsJson(testDB, 465611);
failpoint.off();

// "Index build: joined after abort".
checkLog.containsJson(testDB, 20655);

awaitIndex();
awaitDrop();

MongoRunner.stopMongod(conn);
})();
