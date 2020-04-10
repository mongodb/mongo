/**
 * Tests that the "drop" command can abort in-progress index builds.
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(conn)) {
    jsTest.log("Not running because two phase index builds are not supported.");
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = "drop_collection_aborts_in_progress_index_builds";
const collName = "test";

TestData.dbName = dbName;
TestData.collName = collName;

const testDB = conn.getDB(dbName);
testDB.getCollection(collName).drop();

assert.commandWorked(testDB.createCollection(collName));
const coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({b: 1}));

assert.commandWorked(coll.createIndex({a: 1}));

jsTest.log("Starting two index builds and freezing them.");
IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), coll.getFullName(), {a: 1, b: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1_b_1");

const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), coll.getFullName(), {b: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "b_1");

jsTest.log("Dropping collection " + dbName + "." + collName + " with in-progress index builds");
const awaitDrop = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({drop: TestData.collName}));
}, conn.port);

try {
    checkLog.contains(testDB.getMongo(), "About to abort all index builders on collection");
} finally {
    IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
}

awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitDrop();

MongoRunner.stopMongod(conn);
}());
