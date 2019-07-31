/**
 * Tests that a backtrace will appear in the $currentOp output if the backtrace option is
 * set to true and there is a latch timeout.
 *
 * @tags: [assumes_read_concern_unchanged, assumes_read_preference_unchanged]
 */
(function() {
"use strict";

const adminDB = db.getSiblingDB("admin");

const getCurrentOp = function() {
    let myUri = adminDB.runCommand({whatsmyuri: 1}).you;
    return adminDB
        .aggregate(
            [
                {$currentOp: {localOps: true, allUsers: false, backtrace: true}},
                {$match: {client: myUri}}
            ],
            {readConcern: {level: "local"}})
        .toArray()[0];
};

assert.commandWorked(db.adminCommand(
    {"configureFailPoint": 'keepDiagnosticCaptureOnFailedLock', "mode": 'alwaysOn'}));
var result = getCurrentOp();

assert(result.hasOwnProperty("waitingForLatch"));
assert(result["waitingForLatch"].hasOwnProperty("timestamp"));
assert(result["waitingForLatch"].hasOwnProperty("captureName"));

assert.commandWorked(
    db.adminCommand({"configureFailPoint": 'keepDiagnosticCaptureOnFailedLock', "mode": 'off'}));
result = getCurrentOp();

assert(!result.hasOwnProperty("waitingForLatch"));
})();
