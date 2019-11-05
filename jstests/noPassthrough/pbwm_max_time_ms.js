/**
 * Tests that ParallelBatchWriterMode lock respects maxTimeMS.
 */
(function() {
"use strict";
load("jstests/libs/wait_for_command.js");

const conn = MongoRunner.runMongod();
let db = conn.getDB("test");

let lockPBWM = startParallelShell(() => {
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 60 * 60,
        lockTarget: "ParallelBatchWriterMode",
        $comment: "PBWM lock sleep"
    }),
                                 ErrorCodes.Interrupted);
}, conn.port);

jsTestLog("Wait for that command to appear in currentOp");
const readID = waitForCommand(
    "PBWM lock", op => (op["command"]["$comment"] == "PBWM lock sleep"), conn.getDB("admin"));

jsTestLog("Operation that takes PBWM lock should timeout");
assert.commandFailedWithCode(db.a.runCommand({insert: "a", documents: [{x: 1}], maxTimeMS: 10}),
                             ErrorCodes.MaxTimeMSExpired);

jsTestLog("Kill the sleep command");
assert.commandWorked(db.killOp(readID));

jsTestLog("Wait for sleep command to finish");
lockPBWM();

MongoRunner.stopMongod(conn);
})();
