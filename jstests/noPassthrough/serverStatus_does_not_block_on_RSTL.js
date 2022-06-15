/**
 * Tests that serverStatus is not blocked by an exclusive RSTL lock. Only enforcing on WT.
 *
 * @tags: [
 *   # Certain serverStatus sections might pivot to taking the RSTL lock if an action is unsupported
 *   # by a non-WT storage engine.
 *   requires_wiredtiger,
 *   requires_sharding,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");  // startParallelShell
load("jstests/libs/wait_for_command.js");        // waitForCommand

// Use a sharding environment in order to exercise the sharding specific serverStatus sections.
const st = new ShardingTest(
    {mongos: 1, config: 1, shards: 1, rs: {nodes: 1, setParameter: {watchdogPeriodSeconds: 60}}});
const testDB = st.rs0.getPrimary().getDB("test");

jsTestLog("Starting the sleep command in a parallel thread to take the RSTL MODE_X lock");
let rstlXLockSleepJoin = startParallelShell(() => {
    jsTestLog("Parallel Shell: about to start sleep command");
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 60 * 60,
        // RSTL MODE_X lock.
        lockTarget: "RSTL",
        $comment: "RSTL lock sleep"
    }),
                                 ErrorCodes.Interrupted);
}, testDB.getMongo().port);

jsTestLog("Waiting for the sleep command to start and fetch the opID");
const sleepCmdOpID =
    waitForCommand("RSTL lock", op => (op["command"]["$comment"] == "RSTL lock sleep"), testDB);

jsTestLog("Wait for the sleep command to log that the RSTL MODE_X lock was acquired");
checkLog.containsJson(testDB, 6001600);

try {
    jsTestLog("Running serverStatus concurrently with the RSTL X lock held by the sleep cmd");
    const serverStatusResult =
        assert.commandWorked(testDB.adminCommand({serverStatus: 1, all: 1, maxTimeMS: 20 * 1000}));
    jsTestLog("ServerStatus results: " + tojson(serverStatusResult));

    // Check that serverStatus includes some non-default sections, as a sanity check.
    assert(serverStatusResult["repl"]);
    assert(serverStatusResult["mirroredReads"]);
} finally {
    jsTestLog("Ensure the sleep cmd releases the lock so that the server can shutdown");
    assert.commandWorked(testDB.killOp(sleepCmdOpID));  // kill the sleep cmd
    rstlXLockSleepJoin();  // wait for the thread running the sleep cmd to finish
}

st.stop();
})();
