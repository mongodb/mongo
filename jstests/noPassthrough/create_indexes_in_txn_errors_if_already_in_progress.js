/**
 * Ensures that a createIndexes command request inside a transaction immediately errors if an
 * existing index build of a duplicate index is already in progress outside of the transaction.
 * @tags: [
 *     uses_transactions,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load('jstests/libs/test_background_ops.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "create_indexes_waits_for_already_in_progress";
const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);
const indexSpecB = {
    key: {b: 1},
    name: "the_b_1_index"
};
const indexSpecC = {
    key: {c: 1},
    name: "the_c_1_index"
};

assert.commandWorked(testDB.runCommand({create: collName}));

const runSuccessfulIndexBuild = function(dbName, collName, indexSpec, requestNumber) {
    jsTest.log("Index build request " + requestNumber + " starting...");
    const res = db.getSiblingDB(dbName).runCommand({createIndexes: collName, indexes: [indexSpec]});
    jsTest.log("Index build request " + requestNumber +
               ", expected to succeed, result: " + tojson(res));
    assert.commandWorked(res);
};

const runFailedIndexBuildInTxn = function(dbName, collName, indexSpec, requestNumber) {
    const session = db.getMongo().startSession();

    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];
    jsTest.log("Index build request " + requestNumber + " starting in a transaction...");
    session.startTransaction();
    const res = sessionColl.runCommand({createIndexes: collName, indexes: [indexSpec]});
    jsTest.log("Index build request " + requestNumber +
               ", expected to fail, result: " + tojson(res));
    assert.commandFailedWithCode(res, ErrorCodes.IndexBuildAlreadyInProgress);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
};

// Insert document into collection to avoid optimization for index creation on an empty collection.
// This allows us to pause index builds on the collection using a fail point.
assert.commandWorked(testColl.insert({a: 1}));

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));
let joinFirstIndexBuild;
let joinSecondIndexBuild;
try {
    jsTest.log("Starting a parallel shell to run first index build request...");
    joinFirstIndexBuild = startParallelShell(
        funWithArgs(runSuccessfulIndexBuild, dbName, collName, indexSpecB, 1), primary.port);

    jsTest.log("Waiting for first index build to get started...");
    checkLog.contains(primary,
                      "Hanging index build due to failpoint 'hangAfterSettingUpIndexBuild'");

    jsTest.log(
        "Starting a parallel shell to run a transaction with a second index build request...");
    joinSecondIndexBuild = startParallelShell(
        funWithArgs(runFailedIndexBuildInTxn, dbName, collName, indexSpecB, 2), primary.port);
    // We wait to observe the second attempt to build the index fails while the
    // hangAfterSettingUpIndexBuild is preventing the first attempt from completing successfully.
    joinSecondIndexBuild();
} finally {
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'}));
}

joinFirstIndexBuild();

// We should have the _id index and the 'the_b_1_index' index just built.
assert.eq(testColl.getIndexes().length, 2);
rst.stopSet();
})();
