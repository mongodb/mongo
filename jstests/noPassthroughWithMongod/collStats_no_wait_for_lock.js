/**
 * Tests that when run with waitForLock=false, collStats does not conflict with a collection MODE_X
 * lock.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");

const dbName = "test";
const collName = "collStats_no_wait_for_lock";

const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

const sleepFunction = function(dbName, collName) {
    // If collStats needs to wait on this lock, holding this lock for 4 hours will trigger a test
    // timeout.
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 18000,
        lockTarget: dbName + "." + collName,
        lock: "w",
        $comment: "Lock sleep"
    }),
                                 ErrorCodes.Interrupted);
};

assert.commandWorked(testColl.insert({a: 1}));

const sleepCommand =
    startParallelShell(funWithArgs(sleepFunction, dbName, collName), testDB.getMongo().port);
const sleepID =
    waitForCommand("sleepCmd",
                   op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"),
                   testDB.getSiblingDB("admin"));

// The collStats result should not contain any statistics since we are unable to acquire the lock.
// The only fields present in the result should be 'ok' and 'ns'.
const res = assert.commandWorked(testDB.runCommand({collStats: collName, waitForLock: false}));
assert(res.hasOwnProperty("ns"));
assert.eq(Object.keys(res).length, 2);

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));
sleepCommand();
})();