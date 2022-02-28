/**
 * Tests that when run with waitForLock=false, collStats does not conflict with a global MODE_X
 * lock, but rather returns early.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");

const dbName = "test";
const collName = "collStats_no_wait_for_lock";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({a: 1}));

jsTestLog("Starting the sleep command to take locks");
let globalXLockSleepJoin = startParallelShell(() => {
    jsTestLog("Parallel Shell: about to start sleep command");
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 18000,
        // Global MODE_X lock (and RSTL MODE_IX).
        w: true,
        $comment: "Global lock sleep"
    }),
                                 ErrorCodes.Interrupted);
}, testDB.getMongo().port);

jsTestLog("Waiting for the sleep command to start and fetch the opID");
const sleepCmdOpID = waitForCommand(
    "sleepCmd",
    op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Global lock sleep"),
    testDB.getSiblingDB("admin"));

jsTestLog("Wait for the sleep command to log that the Global lock was acquired");
checkLog.containsJson(testDB, 6001601);

try {
    jsTestLog("Running collStats concurrently with the global X lock");
    // The collStats result should not contain any statistics since we are unable to acquire the
    // lock. The only fields present in the result should be 'ok' and 'ns', since 'waitForLock' is
    // set to false.
    const res = assert.commandWorked(
        testDB.runCommand({collStats: collName, waitForLock: false, maxTimeMS: 20 * 1000}));
    assert(res.hasOwnProperty("ns"));
    assert.eq(Object.keys(res).length, 2);
} finally {
    jsTestLog("Ensure the sleep cmd releases the lock so that the server can shutdown");
    assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepCmdOpID));  // kill the sleep cmd
    globalXLockSleepJoin();  // wait for the thread running the sleep cmd to finish
}
})();
