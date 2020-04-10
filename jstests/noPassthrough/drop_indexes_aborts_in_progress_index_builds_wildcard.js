/**
 * Tests that the "dropIndexes" command can abort in-progress index builds. The "dropIndexes"
 * command will only abort in-progress index builds if the user specifies all of the indexes that a
 * single builder is building together, as we can only abort at the index builder granularity level.
 *
 * In this file, we test calling "dropIndexes" with the "*" wildcard which will abort all
 * in-progress index builds and remove ready indexes.
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);

const dbName = "drop_indexes_aborts_in_progress_index_builds_wildcard";
const collName = "test";

TestData.dbName = dbName;
TestData.collName = collName;

const testDB = conn.getDB(dbName);
testDB.getCollection(collName).drop();

assert.commandWorked(testDB.createCollection(collName));
const coll = testDB.getCollection(collName);

jsTest.log("Aborting all index builders and ready indexes with '*' wildcard");
assert.commandWorked(testDB.getCollection(collName).insert({a: 1}));
assert.commandWorked(testDB.getCollection(collName).insert({b: 1}));

assert.commandWorked(testDB.getCollection(collName).createIndex({a: 1}));

IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), coll.getFullName(), {a: 1, b: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1_b_1");

const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), coll.getFullName(), {b: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "b_1");

const awaitDropIndex = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({dropIndexes: TestData.collName, index: "*"}));
}, conn.port);

checkLog.contains(testDB.getMongo(), "About to abort all index builders on collection");
IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitDropIndex();

assert.eq(1, testDB.getCollection(collName).getIndexes().length);

MongoRunner.stopMongod(conn);
}());
