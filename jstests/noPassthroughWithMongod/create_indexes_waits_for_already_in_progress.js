/**
 * Tests that a second duplicate createIndexes cmd request will wait for the first createIndexes cmd
 * request to finish before proceeding to either: return OK; or try to build the index again.
 *
 * Sets up paused index builds via failpoints and a parallel shell.
 *
 * First tests that the second request returns OK after finding the index ready after waiting;
 * then tests that the second request builds the index after waiting and finding the index does
 * not exist.
 *
 * @tags: [
 *     # Uses failpoints that the mongos does not have.
 *     assumes_against_mongod_not_mongos,
 *     # Sets a failpoint on one mongod, so switching primaries would break the test.
 *     does_not_support_stepdowns,
 *     # The ephemeralForTest engine has collection level locking, meaning that it upgrades
 *     # collection intent locks to exclusive. This test depends on two concurrent ops taking
 *     # concurrent collection IX locks.
 *     requires_document_locking,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load('jstests/libs/test_background_ops.js');

const dbName = "test";
const collName = "create_indexes_waits_for_already_in_progress";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);
const indexSpecB = {
    key: {b: 1},
    name: "the_b_1_index"
};
const indexSpecC = {
    key: {c: 1},
    name: "the_c_1_index"
};

testColl.drop();
assert.commandWorked(testDB.adminCommand({clearLog: 'global'}));
// This test depends on using the IndexBuildsCoordinator to build this index, which as of
// SERVER-44405, will not occur in this test unless the collection is created beforehand.
assert.commandWorked(testDB.runCommand({create: collName}));

// Insert document into collection to avoid optimization for index creation on an empty collection.
// This allows us to pause index builds on the collection using a fail point.
assert.commandWorked(testColl.insert({a: 1}));

function runSuccessfulIndexBuild(dbName, collName, indexSpec, requestNumber) {
    jsTest.log("Index build request " + requestNumber + " starting...");
    const res = db.getSiblingDB(dbName).runCommand({createIndexes: collName, indexes: [indexSpec]});
    jsTest.log("Index build request " + requestNumber +
               ", expected to succeed, result: " + tojson(res));
    assert.commandWorked(res);
}

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));
let joinFirstIndexBuild;
let joinSecondIndexBuild;
try {
    jsTest.log("Starting a parallel shell to run first index build request...");
    joinFirstIndexBuild = startParallelShell(
        funWithArgs(runSuccessfulIndexBuild, dbName, collName, indexSpecB, 1), db.getMongo().port);

    jsTest.log("Waiting for first index build to get started...");
    checkLog.contains(db.getMongo(),
                      "Hanging index build due to failpoint 'hangAfterSettingUpIndexBuild'");

    jsTest.log("Starting a parallel shell to run second index build request...");
    joinSecondIndexBuild = startParallelShell(
        funWithArgs(runSuccessfulIndexBuild, dbName, collName, indexSpecB, 2), db.getMongo().port);

    jsTest.log("Waiting for second index build request to wait behind the first...");
    checkLog.contains(db.getMongo(),
                      "but found that at least one of the indexes is already being built");
} finally {
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'}));
}

// The second request stalled behind the first, so now all we need to do is check that they both
// complete successfully.
joinFirstIndexBuild();
joinSecondIndexBuild();

// Make sure the parallel shells sucessfully built the index. We should have the _id index and
// the 'the_b_1_index' index just built in the parallel shells.
assert.eq(testColl.getIndexes().length, 2);

// Lastly, if the first request fails transiently, then the second should restart the index
// build.
assert.commandWorked(testDB.adminCommand({clearLog: 'global'}));

function runFailedIndexBuild(dbName, collName, indexSpec, requestNumber) {
    const res = db.getSiblingDB(dbName).runCommand({createIndexes: collName, indexes: [indexSpec]});
    jsTest.log("Index build request " + requestNumber +
               ", expected to fail, result: " + tojson(res));
    assert.commandFailedWithCode(res, 4698903);
}

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));
let joinFailedIndexBuild;
let joinSuccessfulIndexBuild;
try {
    jsTest.log("Starting a parallel shell to run third index build request...");
    joinFailedIndexBuild = startParallelShell(
        funWithArgs(runFailedIndexBuild, dbName, collName, indexSpecC, 3), db.getMongo().port);

    jsTest.log("Waiting for third index build to get started...");
    checkLog.contains(db.getMongo(),
                      "Hanging index build due to failpoint 'hangAfterSettingUpIndexBuild'");

    jsTest.log("Starting a parallel shell to run fourth index build request...");
    joinSuccessfulIndexBuild = startParallelShell(
        funWithArgs(runSuccessfulIndexBuild, dbName, collName, indexSpecC, 4), db.getMongo().port);

    jsTest.log("Waiting for fourth index build request to wait behind the third...");
    checkLog.contains(db.getMongo(),
                      "but found that at least one of the indexes is already being built");

    jsTest.log("Failing third index build");
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'failIndexBuildOnCommit', mode: {times: 1}}));
} finally {
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'}));
}

// The second request stalled behind the first, so now all we need to do is check that they both
// complete as expected: the first should fail; the second should succeed.
joinFailedIndexBuild();
joinSuccessfulIndexBuild();

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'failIndexBuildOnCommit', mode: 'off'}));

// Make sure the parallel shells sucessfully built the index. We should now have the _id index,
// the 'the_b_1_index' index and the 'the_c_1_index' just built in the parallel shells.
assert.eq(testColl.getIndexes().length, 3);
})();
