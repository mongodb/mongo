/**
 * Tests that the "dropIndexes" command can abort in-progress index builds. The "dropIndexes"
 * command will only abort in-progress index builds if the user specifies all of the indexes that a
 * single builder is building together, as we can only abort at the index builder granularity level.
 *
 * In this file, we test calling "dropIndexes" with a key pattern whose index build is in-progress.
 */
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);

const dbName = jsTestName();
const collName = "test";

TestData.dbName = dbName;
TestData.collName = collName;

const testDB = conn.getDB(dbName);
testDB.getCollection(collName).drop();

assert.commandWorked(testDB.createCollection(collName));
const coll = testDB.getCollection(collName);

jsTest.log("Aborting index builder by key pattern");
assert.commandWorked(testDB.getCollection(collName).insert({a: 1}));
assert.commandWorked(testDB.getCollection(collName).insert({b: 1}));

IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), coll.getFullName(), {a: 1, b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1_b_1");

const awaitDropIndex = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({dropIndexes: TestData.collName, index: {a: 1, b: 1}}));
}, conn.port);

checkLog.contains(testDB.getMongo(), "About to abort index builder");
IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
awaitIndexBuild();
awaitDropIndex();

assert.eq(1, testDB.getCollection(collName).getIndexes().length);

MongoRunner.stopMongod(conn);
