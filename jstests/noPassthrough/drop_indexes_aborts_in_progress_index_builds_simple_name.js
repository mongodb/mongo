/**
 * Tests that the "dropIndexes" command can abort in-progress index builds. The "dropIndexes"
 * command will only abort in-progress index builds if the user specifies all of the indexes that a
 * single builder is building together, as we can only abort at the index builder granularity level.
 *
 * This test also confirms that secondary reads are supported while index builds are in progress.
 *
 * In this file, we test calling "dropIndexes" with a simple index name whose index build is
 * in-progress.
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");
load('jstests/replsets/libs/secondary_reads_test.js');

const dbName = "drop_indexes_aborts_in_progress_index_builds_simple_name";

const secondaryReadsTest = new SecondaryReadsTest(dbName);

let primaryDB = secondaryReadsTest.getPrimaryDB();
const conn = primaryDB.getMongo();

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(conn)) {
    jsTestLog('Two phase index builds not enabled, skipping test.');
    secondaryReadsTest.stop();
    return;
}

const collName = "test";

TestData.dbName = dbName;
TestData.collName = collName;

const testDB = conn.getDB(dbName);
testDB.getCollection(collName).drop();

assert.commandWorked(testDB.createCollection(collName));
const coll = testDB.getCollection(collName);

jsTest.log("Aborting index builder with one index build and simple index spec");
assert.commandWorked(testDB.getCollection(collName).insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(testDB.getMongo());
IndexBuildTest.pauseIndexBuilds(secondaryReadsTest.getSecondaryDB().getMongo());

const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    testDB.getMongo(), coll.getFullName(), {a: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "a_1");
IndexBuildTest.waitForIndexBuildToStart(secondaryReadsTest.getSecondaryDB(), collName, "a_1");

// Test secondary reads during oplog application.
// Prevent a batch from completing on the secondary.
const pauseAwait = secondaryReadsTest.pauseSecondaryBatchApplication();

for (let i = 100; i < 200; i++) {
    assert.commandWorked(testDB.getCollection(collName).insert({a: i}));
}

// Wait for the batch application to pause.
pauseAwait();

// Do a bunch of reads on the 'collName' collection on the secondary.
// No errors should be encountered on the secondary.
let readFn = function() {
    for (let x = 0; x < TestData.nOps; x++) {
        assert.commandWorked(db.runCommand({
            find: TestData.collName,
            filter: {a: x},
        }));
        // Sleep a bit to make these reader threads less CPU intensive.
        sleep(60);
    }
};
TestData.nOps = 10;
const nReaders = 3;
secondaryReadsTest.startSecondaryReaders(nReaders, readFn);

// Disable the failpoint and let the batch complete.
secondaryReadsTest.resumeSecondaryBatchApplication();
secondaryReadsTest.stopReaders();

const awaitDropIndex = startParallelShell(() => {
    const testDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(testDB.runCommand({dropIndexes: TestData.collName, index: "a_1"}));
}, conn.port);

checkLog.contains(testDB.getMongo(), "About to abort index builder");
IndexBuildTest.resumeIndexBuilds(testDB.getMongo());
IndexBuildTest.resumeIndexBuilds(secondaryReadsTest.getSecondaryDB().getMongo());
awaitIndexBuild();
awaitDropIndex();

assert.eq(1, testDB.getCollection(collName).getIndexes().length);

secondaryReadsTest.stop();
}());
