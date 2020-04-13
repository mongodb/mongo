/*
 * Tests that we don't hit 3 way deadlock between an index builder, prepared transaction, and step
 * down.
 *
 * This tests the following scenario:
 * 1) Starts and index build.
 * 2) Prepares a transaction which holds the collection lock in IX mode.
 * 3) Waits for the index build to attempt to acquire the collection lock in S mode to stop writes,
 *    but blocks behind the prepared transaction due to a collection lock conflict.
 * 4) Steps down the primary, which enqueues the RSTL in X mode.
 * 5) Ensures the index build has released its RSTL lock before taking the MODE_S lock, and does not
 *    block stepDown.
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

jsTestLog("Do a document write");
assert.commandWorked(primaryColl.insert({_id: 1, x: 1}, {"writeConcern": {"w": 1}}));

// Clear the log.
assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

// Enable fail point which makes the index build to hang before taking a MODE_S lock to block
// writes.
const failPoint = "hangAfterIndexBuildDumpsInsertsFromBulk";
let res =
    assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
let timesEntered = res.count;

const indexThread = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {x: 1}, {}, ErrorCodes.InterruptedDueToReplStateChange);

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

// Unblock the index build, which will cause it to hang acquiring the collection S lock.
assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "off"}));

const stepDownThread = startParallelShell(() => {
    jsTestLog("Make primary step down");
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60 * 60, "force": true}));
}, primary.port);

jsTestLog("Waiting for stepdown to complete");
indexThread();
stepDownThread();

waitForState(primary, ReplSetTest.State.SECONDARY);
// Allow the primary to be re-elected, and wait for it.
assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
rst.getPrimary();

jsTestLog("Aborting transaction and waiting for index build to finish");
assert.commandWorked(session.abortTransaction_forTesting());
IndexBuildTest.waitForIndexBuildToStop(primaryDB, primaryColl.getFullName(), "x_1");

// A single-phase index build will get aborted from the state transition.
if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "x_1"], []);
} else {
    IndexBuildTest.assertIndexes(primaryColl, 1, ["_id_"], []);
}

rst.stopSet();
})();
