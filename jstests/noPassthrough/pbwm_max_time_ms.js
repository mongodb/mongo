/**
 * Tests that ParallelBatchWriterMode lock respects maxTimeMS.
 */
(function() {
"use strict";

const waitForCommand = function(conn, waitingFor, opFilter) {
    let opId = -1;
    assert.soon(function() {
        print(`Checking for ${waitingFor}`);
        const curopRes = conn.getDB("admin").currentOp();
        assert.commandWorked(curopRes);
        const foundOp = curopRes["inprog"].filter(opFilter);

        if (foundOp.length == 1) {
            opId = foundOp[0]["opid"];
        }
        return (foundOp.length == 1);
    });
    return opId;
};

const conn = MongoRunner.runMongod();
let db = conn.getDB("test");

let lockPBWM = startParallelShell(() => {
    db.adminCommand(
        {sleep: 1, secs: 20, lockTarget: "ParallelBatchWriterMode", $comment: "PBWM lock sleep"});
}, conn.port);

jsTestLog("Wait for that command to appear in currentOp");
const readID =
    waitForCommand(conn, "PBWM lock", op => (op["command"]["$comment"] == "PBWM lock sleep"));

jsTestLog("Operation that takes PBWM lock should timeout");
assert.commandFailedWithCode(db.a.runCommand({insert: "a", documents: [{x: 1}], maxTimeMS: 10}),
                             ErrorCodes.MaxTimeMSExpired);

jsTestLog("Wait for sleep command to finish");
lockPBWM();

MongoRunner.stopMongod(conn);
})();
