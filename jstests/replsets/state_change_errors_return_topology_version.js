/**
 * This tests that state change errors include a TopologyVersion field.
 *
 * @tags: [requires_fcv_44]
 */

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "state_change_errors_return_topology_version";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);

const stateChangeErrorCodes = [
    ErrorCodes.InterruptedAtShutdown,
    ErrorCodes.InterruptedDueToReplStateChange,
    ErrorCodes.ShutdownInProgress,
    ErrorCodes.NotMaster,
    ErrorCodes.NotMasterNoSlaveOk,
    ErrorCodes.NotMasterOrSecondary,
    ErrorCodes.PrimarySteppedDown
];

function runFailInCommandDispatch(errorCode, isWCError) {
    jsTestLog(`Testing with errorCode: ${
        errorCode} with failPoint: failWithErrorCodeInCommandDispatch, isWCError: ${isWCError}.`);

    if (isWCError) {
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                writeConcernError: {code: NumberInt(errorCode), errmsg: "dummy"},
                failCommands: ["insert"],
            }
        }));
    } else {
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                errorCode: NumberInt(errorCode),
                failCommands: ["insert"],
            }
        }));
    }

    const res = primaryDB.runCommand({insert: collName, documents: [{x: 1}]});
    assert.commandFailedWithCode(res, errorCode);
    // Only StateChangeErrors should return TopologyVersion in the response.
    if (stateChangeErrorCodes.includes(errorCode)) {
        assert(res.hasOwnProperty("topologyVersion"), tojson(res));
    } else {
        assert(!res.hasOwnProperty("topologyVersion"), tojson(res));
    }
    assert.commandWorked(primary.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
}

function runFailInRunCommand(errorCode) {
    jsTestLog(
        `Testing with errorCode: ${errorCode} with failPoint: failWithErrorCodeInRunCommand.`);

    assert.commandWorked(primary.adminCommand({
        configureFailPoint: "failWithErrorCodeInRunCommand",
        mode: "alwaysOn",
        data: {
            errorCode: NumberInt(errorCode),
        }
    }));

    const res = primaryDB.runCommand({insert: collName, documents: [{x: 1}]});
    assert.commandFailedWithCode(res, errorCode);
    // Only StateChangeErrors should return TopologyVersion in the response.
    if (stateChangeErrorCodes.includes(errorCode)) {
        assert(res.hasOwnProperty("topologyVersion"), tojson(res));
    } else {
        assert(!res.hasOwnProperty("topologyVersion"), tojson(res));
    }
    // We expect 'configureFailPoint' to succeed here since the 'failWithErrorCodeInRunCommand' fail
    // point only affects commands that support writeConcern.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "failWithErrorCodeInRunCommand", mode: "off"}));
}

stateChangeErrorCodes.forEach(function(code) {
    runFailInCommandDispatch(code, true /* isWCError */);
    runFailInCommandDispatch(code, false /* isWCError */);
});

// Test that errors that are not state change errors will not return a TopologyVersion.
runFailInCommandDispatch(ErrorCodes.InternalError, true /* isWCError */);
runFailInCommandDispatch(ErrorCodes.InternalError, false /* isWCError */);

stateChangeErrorCodes.forEach(function(code) {
    runFailInRunCommand(code);
});
runFailInRunCommand(ErrorCodes.InternalError);
rst.stopSet();
})();
