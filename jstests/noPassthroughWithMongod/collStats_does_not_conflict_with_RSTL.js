/**
 * Tests that collStats can run concurrently with a RSTL MODE_X lock.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");  // startParallelShell
load("jstests/libs/wait_for_command.js");        // waitForCommand

const dbName = "test";
const collName = "collStats_no_wait_for_lock";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({a: 1}));

jsTestLog("Starting the sleep command to take locks");
let rstlXLockSleepJoin = startParallelShell(() => {
    jsTestLog("Parallel Shell: about to start sleep command");
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 18000,
        // RSTL MODE_X lock.
        lockTarget: "RSTL",
        $comment: "RSTL lock sleep"
    }),
                                 ErrorCodes.Interrupted);
}, testDB.getMongo().port);

jsTestLog("Waiting for the sleep command to start and fetch the opID");
const sleepCmdOpID = waitForCommand(
    "sleepCmd",
    op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "RSTL lock sleep"),
    testDB.getSiblingDB("admin"));

jsTestLog("Wait for the sleep command to log that the RSTL MODE_X lock was acquired");
checkLog.containsJson(testDB, 6001600);

try {
    jsTestLog("Running collStats concurrently with the RSTL X lock");
    assert.commandWorked(testDB.runCommand({collStats: collName, maxTimeMS: 20 * 1000}));
} finally {
    jsTestLog("Ensure the sleep cmd releases the lock so that the server can shutdown");
    assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepCmdOpID));  // kill the sleep cmd
    rstlXLockSleepJoin();  // wait for the thread running the sleep cmd to finish
}
})();
