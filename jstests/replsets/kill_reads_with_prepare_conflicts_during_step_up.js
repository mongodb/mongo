/*
 * Tests that a read operation on a secondary that encounters a prepare conflict gets killed
 * when we cause the secondary to step up.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/check_log.js");

const rst = new ReplSetTest({nodes: 2});
rst.startSet();

const config = rst.getReplSetConfig();
// Increase the election timeout so that we do not accidentally trigger an election before
// we make the secondary step up.
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
rst.initiate(config);

let primary = rst.getPrimary();
let secondary = rst.getSecondary();

const dbName = "test";
const collName = "kill_reads_with_prepare_conflicts_during_step_up";

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

let session = primary.startSession();
const sessionID = session.getSessionId();
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "WTPrintPrepareConflictLog", mode: "alwaysOn"}));

// Insert a document that we will later modify in a transaction.
assert.commandWorked(primaryColl.insert({_id: 1}));

jsTestLog("Start a transaction and prepare it");
session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1}, {_id: 1, a: 1}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Advance the clusterTime with another insert.
const clusterTimeAfterPrepare =
    assert
        .commandWorked(primaryColl.runCommand(
            "insert", {documents: [{advanceClusterTime: 1}], writeConcern: {w: "majority"}}))
        .operationTime;

// Ensure that the secondary replicates the prepare and the additional insert.
rst.awaitReplication();

// Make sure a secondary read using afterClusterTime times out when trying to
// read a prepared document.
const secondaryDB = secondary.getDB(dbName);
assert.commandFailedWithCode(secondaryDB.runCommand({
    find: collName,
    filter: {_id: 1},
    readConcern: {afterClusterTime: clusterTimeAfterPrepare},
    maxTimeMS: 2 * 1000  // 2 seconds
}),
                             ErrorCodes.MaxTimeMSExpired);

// Clear secondary log so that when we wait for the WTPrintPrepareConflictLog fail point, we
// do not count the previous find.
assert.commandWorked(secondaryDB.adminCommand({clearLog: "global"}));

TestData.dbName = dbName;
TestData.collName = collName;
TestData.clusterTime = clusterTimeAfterPrepare;

const waitForSecondaryReadBlockedOnPrepareConflictThread = startParallelShell(() => {
    // Allow for secondary reads.
    db.getMongo().setSlaveOk();
    const parallelTestDB = db.getSiblingDB(TestData.dbName);
    const parallelTestCollName = TestData.collName;

    // The following read should block on the prepared transaction since it will be
    // reading a conflicting document using an afterClusterTime later than the
    // prepareTimestamp.
    assert.commandFailedWithCode(parallelTestDB.runCommand({
        find: parallelTestCollName,
        filter: {_id: 1},
        readConcern: {afterClusterTime: TestData.clusterTime}
    }),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}, secondary.port);

jsTestLog("Waiting for failpoint");
checkLog.contains(secondary, "WTPrintPrepareConflictLog fail point enabled");

// Once we've confirmed that the find command has hit a prepare conflict on the secondary, cause
// that secondary to step up.
jsTestLog("Stepping up secondary");
rst.stepUp(secondary);

waitForSecondaryReadBlockedOnPrepareConflictThread();

rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
rst.waitForState(primary, ReplSetTest.State.SECONDARY);

primary = rst.getPrimary();

// Validate that the read operation got killed during step up.
let replMetrics = assert.commandWorked(primary.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "stepUp");
assert.eq(replMetrics.stateTransition.userOperationsKilled, 1);

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