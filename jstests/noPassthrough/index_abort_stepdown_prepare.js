/*
 * Tests that we don't hit 3 way deadlock between a interrupted index build, prepared txn and step
 * down thread.
 *
 * This test creates a scenario where:
 * 1) Starts an index build.
 * 2) Transaction gets prepared and holds the collection lock in MODE_IX.
 * 3) A dropIndexes command attempts to abort the createIndexes command. The abort command holds the
 *    RSTL lock in MODE_IX and tries to acquire a MODE_X collection lock, but blocks on the prepared
 *    transaction.
 * 4) Step down enqueues RSTL in MODE_X and waits for aborting thread to release RSTL lock.
 * 5) The aborting thread gets interrupted by step down, step down completes, and the index build
 *    eventually completes on the new primary.
 *
 * @tags: [
 *     uses_transactions,
 *     uses_prepare_transaction,
 * ]
 */
load('jstests/noPassthrough/libs/index_build.js');
load("jstests/replsets/rslib.js");
load("jstests/core/txns/libs/prepare_helpers.js");

(function() {

"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

TestData.dbName = dbName;
TestData.collName = collName;

jsTestLog("Do a document write");
assert.commandWorked(primaryColl.insert({_id: 1, a: 1}));

// Enable fail point which makes index build hang in an interruptible state.
const failPoint = "hangAfterIndexBuildDumpsInsertsFromBulk";
let res =
    assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
let timesEntered = res.count;

const indexName = "a_1";
const createIndex = IndexBuildTest.startIndexBuild(primary,
                                                   primaryColl.getFullName(),
                                                   {a: 1},
                                                   {name: indexName},
                                                   ErrorCodes.InterruptedDueToReplStateChange);
jsTestLog("Waiting for index build to hit failpoint");
assert.commandWorked(primary.adminCommand({
    waitForFailPoint: failPoint,
    timesEntered: timesEntered + 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Start txn");
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1}, {$set: {y: 1}}));

jsTestLog("Prepare txn");
PrepareHelpers.prepareTransaction(session);

// Attempt to abort the index build. It will hang holding the RSTL while waiting for the collection
// X lock.
const abortIndexThread = startParallelShell(() => {
    jsTestLog("Attempting to abort the index build");
    assert.commandFailedWithCode(db.getSiblingDB('test').coll.dropIndex("a_1"),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}, primary.port);
checkLog.containsJson(primary, 4656010);

// Stepdown should interrupt the dropIndexes operation and cause it to drop its queued lock.
jsTestLog("Stepping down the primary");
assert.commandWorked(primaryDB.adminCommand({"replSetStepDown": 5 * 60, "force": true}));

// Unblock the index build and wait for threads to join. The stepdown should succeed in unblocking
// the abort. In the case of single-phase index builds, the abort will succeed after the stepdown.
assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "off"}));
abortIndexThread();
createIndex();

jsTestLog("Waiting for node to become secondary");
waitForState(primary, ReplSetTest.State.SECONDARY);
// Allow the primary to be re-elected, and wait for it.

assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
rst.getPrimary();

jsTestLog("Abort txn");
assert.commandWorked(session.abortTransaction_forTesting());

jsTestLog("Waiting for index build to complete");
IndexBuildTest.waitForIndexBuildToStop(primaryDB, primaryColl.getName(), indexName);

if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", indexName]);
} else {
    // Single-phase index builds are aborted on step-down.
    IndexBuildTest.assertIndexes(primaryColl, 1, ["_id_"]);
}

rst.stopSet();
})();
