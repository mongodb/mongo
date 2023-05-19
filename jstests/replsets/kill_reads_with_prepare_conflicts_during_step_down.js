/*
 * Tests that read operations that encounter prepare conflicts are killed during
 * stepdown to prevent deadlocks between prepare conflicts and state transitions.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");

// Start one of the nodes with priority: 0 to avoid elections.
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const dbName = "test";
const collName = "kill_reads_with_prepare_conflicts_during_step_down";

const primaryDB = primary.getDB(dbName);
// Used to make sure that the correct amount of operations were killed on this node
// during stepdown.
const primaryAdmin = primary.getDB("admin");
const primaryColl = primaryDB[collName];

let session = primary.startSession();
const sessionID = session.getSessionId();
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

let failPoint = configureFailPoint(primaryAdmin, "WTPrintPrepareConflictLog");

// Insert a document that we will later modify in a transaction.
assert.commandWorked(primaryColl.insert({_id: 1}));

jsTestLog("Start a transaction and prepare it");
session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1}, {_id: 1, a: 1}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

TestData.dbName = dbName;
TestData.collName = collName;

const readBlockedOnPrepareConflictThread = startParallelShell(() => {
    const parallelTestDB = db.getSiblingDB(TestData.dbName);
    const parallelTestCollName = TestData.collName;

    // Advance the clusterTime with another insert.
    let res = assert.commandWorked(parallelTestDB.runCommand(
        {insert: parallelTestCollName, documents: [{advanceClusterTime: 1}]}));
    assert(res.hasOwnProperty("$clusterTime"), res);
    assert(res.$clusterTime.hasOwnProperty("clusterTime"), res);
    const clusterTime = res.$clusterTime.clusterTime;
    jsTestLog("Using afterClusterTime: " + clusterTime);

    // The following read should block on the prepared transaction since it will be
    // reading a conflicting document using an afterClusterTime later than the
    // prepareTimestamp.
    assert.commandFailedWithCode(parallelTestDB.runCommand({
        find: parallelTestCollName,
        filter: {_id: 1},
        readConcern: {afterClusterTime: clusterTime}
    }),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}, primary.port);

jsTestLog("Waiting for failpoint");
failPoint.wait();

// Once we have confirmed that the find command has hit a prepare conflict, we can perform
// a step down.
jsTestLog("Stepping down primary");
assert.commandWorked(
    primaryAdmin.adminCommand({replSetStepDown: 60 * 10 /* 10 minutes */, force: true}));

readBlockedOnPrepareConflictThread();

rst.waitForState(primary, ReplSetTest.State.SECONDARY);

// Validate that the read operation got killed during step down.
let replMetrics = assert.commandWorked(primaryAdmin.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "stepDown");
assert.eq(replMetrics.stateTransition.userOperationsKilled, 1);

// Allow the primary to be re-elected, and wait for it.
assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
primary = rst.getPrimary();

// Make sure we can successfully commit the prepared transaction.
jsTestLog("Restoring shell session state");
session = PrepareHelpers.createSessionWithGivenId(primary, sessionID);
sessionDB = session.getDatabase(dbName);
// The transaction on this session should have a txnNumber of 0. We explicitly set this
// since createSessionWithGivenId does not restore the current txnNumber in the shell.
session.setTxnNumber_forTesting(0);
const txnNumber = session.getTxnNumber_forTesting();

jsTestLog("Committing transaction");
// Commit the transaction.
assert.commandWorked(sessionDB.adminCommand({
    commitTransaction: 1,
    commitTimestamp: prepareTimestamp,
    txnNumber: NumberLong(txnNumber),
    autocommit: false,
}));

rst.stopSet();
})();
