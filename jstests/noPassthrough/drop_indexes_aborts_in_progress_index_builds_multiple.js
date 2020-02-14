/**
 * Tests that the "dropIndexes" command can abort in-progress index builds. The "dropIndexes"
 * command will only abort in-progress index builds if the user specifies all of the indexes that a
 * single builder is building together, as we can only abort at the index builder granularity level.
 *
 * In this file, we test calling "dropIndexes" with a list of index names, which will be used to
 * abort a single index builder only, which was building all the given indexes.
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);

const dbName = "drop_indexes_aborts_in_progress_index_builds_multiple";
const collName = "test";

TestData.dbName = dbName;
TestData.collName = collName;

const testDB = conn.getDB(dbName);
testDB.getCollection(collName).drop();

assert.commandWorked(testDB.createCollection(collName));
const coll = testDB.getCollection(collName);

jsTest.log("Aborting index builder with multiple index builds");
assert.commandWorked(testDB.getCollection(collName).insert({a: 1}));
assert.commandWorked(testDB.getCollection(collName).insert({b: 1}));

IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(),
                                                       coll.getFullName(),
                                                       [{a: 1}, {b: 1}, {a: 1, b: 1}],
                                                       {},
                                                       [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1");
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "b_1");
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1_b_1");

const awaitDropIndex = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(
        testDB.runCommand({dropIndexes: TestData.collName, index: ["b_1", "a_1_b_1", "a_1"]}));
}, conn.port);

checkLog.contains(testDB.getMongo(), "About to abort index builder");
IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
awaitIndexBuild();
awaitDropIndex();

assert.eq(1, testDB.getCollection(collName).getIndexes().length);

MongoRunner.stopMongod(conn);
}());
