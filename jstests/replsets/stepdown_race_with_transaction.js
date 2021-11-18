/**
 * Tests that multi-documment transactions no longer race with stepdown over
 * "setAlwaysInterruptAtStepDownOrUp".
 *
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "testdb";
const collName = "testcoll";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

// Insert a document that we will later modify in a transaction.
assert.commandWorked(primaryColl.insert({a: 1}));

// In the first part of the race, we set the 'setAlwaysInterruptAtStepDownOrUp' too late,
// after stepDown is already done interrupting operations.
const txnFpBefore = configureFailPoint(primary, "hangBeforeSettingTxnInterruptFlag");

// The second critical part of the race is when the transaction thread has already passed
// the regular "not primary" checks by the time stepDown has completed and updated
// writability. (This is the reason we check writability again in the accompanying patch.)
const txnFpAfter =
    configureFailPoint(primary, "hangAfterCheckingWritabilityForMultiDocumentTransactions");

jsTestLog("Start the transaction in a parallel shell");
const txn = startParallelShell(() => {
    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase("testdb");
    const sessionColl = sessionDB.getCollection("testcoll");

    session.startTransaction();
    assert.commandFailedWithCode(sessionColl.insert({b: 2}), ErrorCodes.NotWritablePrimary);
}, primary.port);

jsTestLog("Wait on the first transaction fail point");
txnFpBefore.wait();

const stepdownFP = configureFailPoint(primary, "stepdownHangAfterGrabbingRSTL");

jsTestLog("Issue a stepdown in a parallel shell");
const stepdown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({replSetStepDown: 10 * 60, force: true}));
}, primary.port);

jsTestLog("Wait on the stepdown fail point");
stepdownFP.wait();

// The txn will be forced to wait for stepdown to finish.
jsTestLog("Release the first transaction fail point and wait in the second");
txnFpBefore.off();
txnFpAfter.wait();

jsTestLog("Let stepdown finish");
stepdownFP.off();
stepdown();

jsTestLog("Wait on the second transaction fail point");
txnFpAfter.wait();

jsTestLog("Let the transaction attempt finish");
txnFpAfter.off();
txn();

jsTestLog("Checking that the transaction never succeeded");
assert.eq(1, primaryColl.find().toArray().length);

rst.stopSet();
})();
