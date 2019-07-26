/**
 * Tests that writes which complete before stepdown correctly report their errors after the
 * stepdown.
 */
(function() {
"use strict";

load("jstests/libs/check_log.js");

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryAdmin = primary.getDB("admin");
// We need a separate connection to avoid interference with the ReplSetTestMechanism.
const primaryDataConn = new Mongo(primary.host);
const primaryDb = primaryDataConn.getDB("test");
const collname = "last_error_reported_after_stepdown";
const coll = primaryDb[collname];

// Never retry on network error, because this test needs to detect the network error.
TestData.skipRetryOnNetworkError = true;

// This is specifically testing unacknowledged legacy writes.
primaryDataConn.forceWriteMode('legacy');

assert.commandWorked(
    coll.insert([{_id: 'deleteme'}, {_id: 'updateme', nullfield: null}, {_id: 'findme'}],
                {writeConcern: {w: 1}}));
rst.awaitReplication();

// Note that "operation" should always be on primaryDataConn, so the stepdown doesn't clear
// the last error.
function runStepDownTest({description, logMsg, operation, errorCode, nDocs}) {
    jsTestLog(`Trying ${description} on the primary, then stepping down`);
    // We need to make sure the command is complete before stepping down.
    assert.commandWorked(
        primaryAdmin.adminCommand({setParameter: 1, logComponentVerbosity: {command: 1}}));
    operation();
    // Wait for the operation to complete.
    checkLog.contains(primary, logMsg + ' appName: "MongoDB Shell"');
    assert.commandWorked(
        primaryAdmin.adminCommand({setParameter: 1, logComponentVerbosity: {command: 0}}));
    assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    var lastError = assert.commandWorked(primaryDb.runCommand({getLastError: 1}));
    if (typeof (errorCode) == "number")
        assert.eq(
            lastError.code,
            errorCode,
            "Expected error code " + errorCode + ", got lastError of " + JSON.stringify(lastError));
    else {
        assert(!lastError.err, "Expected no error, got lastError of " + JSON.stringify(lastError));
    }
    if (typeof (nDocs) == "number") {
        assert.eq(lastError.n, nDocs, "Wrong number of documents modified or updated");
    }

    // Allow the primary to be re-elected, and wait for it.
    assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
    rst.getPrimary();
}

// Tests which should have no errors.
// Clear log messages to avoid picking up the log of the insertion of the 'deleteme'
// document.
assert.commandWorked(primaryAdmin.adminCommand({clearLog: 'global'}));
runStepDownTest({
    description: "insert",
    logMsg: "insert " + coll.getFullName(),
    operation: () => coll.insert({_id: 0})
});
runStepDownTest({
    description: "update",
    logMsg: "update ",
    operation: () => coll.update({_id: 'updateme'}, {'$inc': {x: 1}}),
    nDocs: 1
});
runStepDownTest({
    description: "remove",
    logMsg: "remove ",
    operation: () => coll.remove({_id: 'deleteme'}),
    nDocs: 1
});

// Tests which should have errors.
// We repeat log messages from tests above, so clear the log first.
assert.commandWorked(primaryAdmin.adminCommand({clearLog: 'global'}));
runStepDownTest({
    description: "insert with error",
    logMsg: "insert " + coll.getFullName(),
    operation: () => coll.insert({_id: 0}),
    errorCode: ErrorCodes.DuplicateKey
});
runStepDownTest({
    description: "update with error",
    logMsg: "update ",
    operation: () => coll.update({_id: 'updateme'}, {'$inc': {nullfield: 1}}),
    errorCode: ErrorCodes.TypeMismatch,
    nDocs: 0
});
runStepDownTest({
    description: "remove with error",
    logMsg: "remove ",
    operation: () => coll.remove({'$nonsense': {x: 1}}),
    errorCode: ErrorCodes.BadValue,
    nDocs: 0
});

rst.stopSet();
})();
