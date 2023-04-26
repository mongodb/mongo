/**
 * Tests that we don't hit 3 way deadlock between an index builder, prepared transaction, and step
 * down.
 *
 * This tests the following scenario:
 * 1) Starts and index build.
 * 2) Prepares a transaction which holds the collection lock in IX mode.
 * 3) Waits for the index build to attempt to acquire the collection lock in X mode to commit, but
 *    blocks behind the prepared transaction due to a collection lock conflict.
 * 4) Steps down the primary, which enqueues the RSTL in X mode.
 * 5) Ensures the index build has released its RSTL lock before taking the X lock, and does not
 *    block stepDown. Since commit must acquire the RSTL to write its oplog entry, ensures that the
 *    index build is able to retry after failing once due to a stepDown.
 * 6) Steps up a new primary. Ensure that the blocked index build on the secondary does not prevent
 *    step-up from ocurring.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
load('jstests/noPassthrough/libs/index_build.js');
load("jstests/replsets/rslib.js");
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");

(function() {

"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

assert.commandWorked(primaryColl.insert({_id: 1, x: 1}));

// Clear the log.
assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

// Enable fail point which makes hybrid index build to hang.
const failPoint = "hangAfterIndexBuildSecondDrain";
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

// Unblock index build, which will cause it to hang acquiring the X lock to commit.
assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "off"}));

let newPrimary = rst.getSecondary();

jsTestLog("Make primary step down");
const stepDownThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60}));
}, primary.port);

// Wait for threads to join.
indexThread();
stepDownThread();

waitForState(primary, ReplSetTest.State.SECONDARY);
assert.neq(primary.port, newPrimary.port);

jsTestLog("Stepping-up new primary");
// assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
rst.stepUp(newPrimary);
waitForState(newPrimary, ReplSetTest.State.PRIMARY);

jsTestLog("Aborting transaction and waiting for index build to finish");
const newSession = new _DelegatingDriverSession(newPrimary, session);
assert.commandWorked(newSession.abortTransaction_forTesting());

IndexBuildTest.waitForIndexBuildToStop(newPrimary.getDB(dbName), collName, "x_1");
IndexBuildTest.waitForIndexBuildToStop(primary.getDB(dbName), collName, "x_1");

IndexBuildTest.assertIndexes(newPrimary.getDB(dbName).getCollection(collName), 2, ["_id_", "x_1"]);
IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "x_1"]);

rst.stopSet();
})();
