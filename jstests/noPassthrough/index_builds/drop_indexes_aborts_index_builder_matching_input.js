/**
 * Tests that the "dropIndexes" command can abort in-progress index builds. The "dropIndexes"
 * command will only abort in-progress index builds if the user specifies all of the indexes that a
 * single builder is building together, as we can only abort at the index builder granularity level.
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

assert.commandWorked(testDB.getCollection(collName).insert({a: 1}));
assert.commandWorked(testDB.getCollection(collName).insert({b: 1}));
assert.commandWorked(testDB.getCollection(collName).insert({c: 1}));

assert.commandWorked(testDB.getCollection(collName).createIndex({a: 1}));

IndexBuildTest.pauseIndexBuilds(testDB.getMongo());

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), coll.getFullName(), {b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "b_1");

const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(testDB.getMongo(), coll.getFullName(), {c: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "c_1");

IndexBuildTest.assertIndexes(
    coll,
    /*numIndexes=*/ 4,
    /*readyIndexes=*/ ["_id_", "a_1"],
    /*notReadyIndexes=*/ ["b_1", "c_1"],
);

// 'dropIndexes' will only abort in-progress index builds if the 'index' parameter specifies all of
// the indexes that a single builder is building together.
assert.commandFailedWithCode(
    testDB.runCommand({dropIndexes: TestData.collName, index: ["a_1", "b_1"]}),
    ErrorCodes.IndexNotFound,
);

assert.commandFailedWithCode(
    testDB.runCommand({dropIndexes: TestData.collName, index: ["a_1", "c_1"]}),
    ErrorCodes.IndexNotFound,
);

assert.commandFailedWithCode(
    testDB.runCommand({dropIndexes: TestData.collName, index: ["b_1", "c_1"]}),
    ErrorCodes.IndexNotFound,
);

IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
awaitFirstIndexBuild();
awaitSecondIndexBuild();

IndexBuildTest.assertIndexes(
    coll,
    /*numIndexes=*/ 4,
    /*readyIndexes=*/ ["_id_", "a_1", "b_1", "c_1"],
    /*notReadyIndexes=*/ [],
);

MongoRunner.stopMongod(conn);
