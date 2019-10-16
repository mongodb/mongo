/*
 * This test makes sure 'find' and 'getMore' commands fail correctly during rollback.
 * @tags: [requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "coll";

// Set up Rollback Test.
let rollbackTest = new RollbackTest();

// Insert a document to be read later.
assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert({}));

let rollbackNode = rollbackTest.transitionToRollbackOperations();

setFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

setFailPoint(rollbackNode, "GetMoreHangBeforeReadLock");

const joinGetMoreThread = startParallelShell(() => {
    db.getMongo().setSlaveOk();
    const cursorID = assert.commandWorked(db.runCommand({"find": "coll", batchSize: 0})).cursor.id;
    // Make sure an outstanding read operation gets killed during rollback even though the read
    // was started before rollback. Outstanding read operations are killed during rollback and
    // their connections are closed shortly after. So we would get either an error
    // (InterruptedDueToReplStateChange) if the error message is sent out and received before
    // the connection is closed or a network error exception.
    try {
        assert.commandFailedWithCode(db.runCommand({"getMore": cursorID, collection: "coll"}),
                                     ErrorCodes.InterruptedDueToReplStateChange);
    } catch (e) {
        assert.includes(e.toString(), "network error while attempting to run command");
    }
}, rollbackNode.port);

const cursorIdToBeReadDuringRollback =
    assert.commandWorked(rollbackNode.getDB(dbName).runCommand({"find": collName, batchSize: 0}))
        .cursor.id;

// Wait for 'getMore' to hang.
checkLog.contains(rollbackNode, "GetMoreHangBeforeReadLock fail point enabled.");

// Start rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

jsTestLog("Reconnecting to " + rollbackNode.host + " after rollback");
reconnect(rollbackNode.getDB(dbName));

// Wait for rollback to hang.
checkLog.contains(rollbackNode, "rollbackHangAfterTransitionToRollback fail point enabled.");

clearFailPoint(rollbackNode, "GetMoreHangBeforeReadLock");

jsTestLog("Wait for 'getMore' thread to join.");
joinGetMoreThread();

jsTestLog("Reading during rollback.");
// Make sure that read operations fail during rollback.
assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand({"find": collName}),
                             ErrorCodes.NotMasterOrSecondary);
assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand(
                                 {"getMore": cursorIdToBeReadDuringRollback, collection: collName}),
                             ErrorCodes.NotMasterOrSecondary);

// Disable the best-effort check for primary-ness in the service entry point, so that we
// exercise the real check for primary-ness in 'find' and 'getMore' commands.
setFailPoint(rollbackNode, "skipCheckingForNotMasterInCommandDispatch");
jsTestLog("Reading during rollback (again with command dispatch checks disabled).");
assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand({"find": collName}),
                             ErrorCodes.NotMasterOrSecondary);
assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand(
                                 {"getMore": cursorIdToBeReadDuringRollback, collection: collName}),
                             ErrorCodes.NotMasterOrSecondary);

clearFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

rollbackTest.transitionToSteadyStateOperations();

const replMetrics = assert.commandWorked(rollbackNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "rollback");
assert(replMetrics.stateTransition.userOperationsRunning,
       () => "Response should have a 'stateTransition.userOperationsRunning' field: " +
           tojson(replMetrics));
assert(replMetrics.stateTransition.userOperationsKilled,
       () => "Response should have a 'stateTransition.userOperationsKilled' field: " +
           tojson(replMetrics));

// Check the replica set.
rollbackTest.stop();
}());
