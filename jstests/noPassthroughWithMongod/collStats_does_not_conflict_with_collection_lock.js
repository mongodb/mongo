/**
 * Tests that collStats can run concurrently with a MODE_X collection lock.
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

const collectionLockSleepFunction = function(dbName, collName) {
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 18000,
        // Collection lock.
        lockTarget: dbName + "." + collName,
        // MODE_X lock.
        lock: "w",
        $comment: "Collection lock sleep"
    }),
                                 ErrorCodes.Interrupted);
};

jsTestLog("Starting the sleep command to take locks");
const collectionXLockSleepJoin = startParallelShell(
    funWithArgs(collectionLockSleepFunction, dbName, collName), testDB.getMongo().port);

jsTestLog("Waiting for the sleep command to start & take locks on the server");
const sleepID = waitForCommand(
    "sleepCmd",
    op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Collection lock sleep"),
    testDB.getSiblingDB("admin"));

try {
    jsTestLog("Running collStats concurrently with the collection X lock");
    assert.commandWorked(testDB.runCommand({collStats: collName, maxTimeMS: 20 * 1000}));
} finally {
    jsTestLog("Ensure the sleep cmd releases the lock so that the server can shutdown");
    assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));  // kill the sleep cmd
    collectionXLockSleepJoin();  // wait for the thread running the sleep cmd to finish
}
})();
