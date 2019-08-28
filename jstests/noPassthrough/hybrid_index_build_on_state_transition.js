/*
 * Tests that we don't hit 3 way deadlock between hybrid index builder, prepared txn
 * and step down thread.
 *
 * This test creates a scenario where:
 * 1) Hybrid index build gets started and holds the RSTL lock in MODE_IX.
 * 2) Transaction gets prepared and holds the collection lock in MODE_IX.
 * 3) Step down enqueues RSTL in MODE_X, interrupts index build and waits for index
 *    builder to release RSTL lock.
 * 4) Index build cleanup tries to acquire collection lock in MODE_X and blocked behind
 *    the prepared transaction due to collection lock conflict.
 * 5) 'abortTransaction' cmd attempts to acquire the RSTL lock in MODE_IX but blocked
 *    behind the step down thread.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
load("jstests/libs/check_log.js");
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
const collNss = primaryColl.getFullName();

TestData.dbName = dbName;
TestData.collName = collName;

jsTestLog("Do a document write");
assert.commandWorked(primaryColl.insert({_id: 1, x: 1}, {"writeConcern": {"w": 1}}));

// Clear the log.
assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

// Enable fail point which makes hybrid index build to hang.
assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "hangAfterIndexBuildDumpsInsertsFromBulk", mode: "alwaysOn"}));

const indexThread = startParallelShell(() => {
    jsTestLog("Create index");
    const primaryDB = db.getSiblingDB(TestData.dbName);
    assert.commandFailedWithCode(primaryDB[TestData.collName].createIndex({"x": 1}),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}, primary.port);

// Wait for hangAfterIndexBuildDumpsInsertsFromBulk fail point to reach.
checkLog.contains(primary, "Hanging after dumping inserts from bulk builder");

jsTestLog("Start txn");
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1}, {$set: {y: 1}}));

jsTestLog("Prepare txn");
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

const stepDownThread = startParallelShell(() => {
    jsTestLog("Make primary to step down");
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60 * 60, "force": true}));
}, primary.port);

// Wait for step down to start the "RstlKillOpThread" thread.
checkLog.contains(primary, "Starting to kill user operations");

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "hangAfterIndexBuildDumpsInsertsFromBulk", mode: "off"}));

// Wait for threads to join.
indexThread();
stepDownThread();

waitForState(primary, ReplSetTest.State.SECONDARY);
// Allow the primary to be re-elected, and wait for it.
assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
rst.getPrimary();

jsTestLog("Abort txn");
assert.commandWorked(session.abortTransaction_forTesting());

rst.stopSet();
})();
