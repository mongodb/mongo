/**
 * Tests that stepdown terminates writes, but does not disconnect connections.
 */
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryAdmin = primary.getDB("admin");
// We need a separate connection to avoid interference with the ReplSetTestMechanism.
const primaryDataConn = new Mongo(primary.host);
const primaryDb = primaryDataConn.getDB("test");
const collname = "no_disconnect_on_stepdown";
const coll = primaryDb[collname];

// Never retry on network error, because this test needs to detect the network error.
TestData.skipRetryOnNetworkError = true;

// Legacy writes will still disconnect, so don't use them.
primaryDataConn.forceWriteMode('commands');

assert.commandWorked(coll.insert([
    {_id: 'update0', updateme: true},
    {_id: 'update1', updateme: true},
    {_id: 'remove0', removeme: true},
    {_id: 'remove1', removeme: true}
]));
rst.awaitReplication();

jsTestLog("Stepping down with no command in progress.  Should not disconnect.");
// If the 'primary' connection is broken on stepdown, this command will fail.
assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
rst.waitForState(primary, ReplSetTest.State.SECONDARY);
// If the 'primaryDataConn' connection was broken during stepdown, this command will fail.
assert.commandWorked(primaryDb.adminCommand({ping: 1}));
// Allow the primary to be re-elected, and wait for it.
assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
rst.getPrimary();

function runStepDownTest({description, failpoint, operation, errorCode}) {
    jsTestLog(`Trying ${description} on a stepping-down primary`);
    assert.commandWorked(primaryAdmin.adminCommand({
        configureFailPoint: failpoint,
        mode: "alwaysOn",
        data: {shouldContinueOnInterrupt: true}
    }));

    errorCode = errorCode || ErrorCodes.InterruptedDueToReplStateChange;
    const writeCommand = `db.getMongo().forceWriteMode("commands");
                              assert.commandFailedWithCode(${operation}, ${errorCode});
                              assert.commandWorked(db.adminCommand({ping:1}));`;

    const waitForShell = startParallelShell(writeCommand, primary.port);
    waitForCurOpByFilter(primaryAdmin, {"msg": failpoint});
    assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    assert.commandWorked(primaryAdmin.adminCommand({configureFailPoint: failpoint, mode: "off"}));
    try {
        waitForShell();
    } catch (ex) {
        print("Failed trying to write or ping in " + description + ", possibly disconnected.");
        throw ex;
    }

    // Validate the number of operations killed on step down and number of failed unacknowledged
    // writes resulted in network disconnection.
    const replMetrics =
        assert.commandWorked(primaryAdmin.adminCommand({serverStatus: 1})).metrics.repl;
    assert.eq(replMetrics.stateTransition.lastStateTransition, "stepDown");
    assert.eq(replMetrics.stateTransition.userOperationsKilled, 1);
    assert.eq(replMetrics.network.notMasterUnacknowledgedWrites, 0);

    // Allow the primary to be re-elected, and wait for it.
    assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
    rst.getPrimary();
}

// Reduce the max batch size so the insert is reliably interrupted.
assert.commandWorked(primaryAdmin.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 2}));
// Make updates and removes yield more often.
assert.commandWorked(
    primaryAdmin.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 3}));

runStepDownTest({
    description: "insert",
    failpoint: "hangWithLockDuringBatchInsert",
    operation: "db['" + collname + "'].insert([{_id:0}, {_id:1}, {_id:2}])"
});

runStepDownTest({
    description: "update",
    failpoint: "hangWithLockDuringBatchUpdate",
    operation: "db['" + collname + "'].update({updateme: true}, {'$set': {x: 1}})"
});
runStepDownTest({
    description: "remove",
    failpoint: "hangWithLockDuringBatchRemove",
    operation: "db['" + collname + "'].remove({removeme: true})"
});
rst.stopSet();
})();
