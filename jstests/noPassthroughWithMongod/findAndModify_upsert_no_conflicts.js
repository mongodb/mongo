/**
 * Tests that findAndModify with upsert=true does not conflict with a collection MODE_IX lock.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");

const collName = "findAndModify_upsert_no_conflicts";
const testDB = db.getSiblingDB("test");
testDB.dropDatabase();

const sleepFunction = function(sleepDB, sleepColl) {
    // If findAndModify calls need to wait on this lock, holding this lock for 4 hours will
    // trigger a test timeout.
    assert.commandFailedWithCode(db.getSiblingDB("test").adminCommand({
        sleep: 1,
        secs: 18000,
        lockTarget: sleepDB + "." + sleepColl,
        lock: "iw",
        $comment: "Lock sleep"
    }),
                                 ErrorCodes.Interrupted);
};

const sleepCommand =
    startParallelShell(funWithArgs(sleepFunction, "test", collName), testDB.getMongo().port);
const sleepID =
    waitForCommand("sleepCmd",
                   op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"),
                   testDB.getSiblingDB("admin"));

const updateDoc = {
    a: 2
};
assert.commandWorked(testDB.runCommand({
    findAndModify: collName,
    query: {a: 1},
    update: updateDoc,
    upsert: true,
    // Set expiration on lock acquisition to avoid waiting indefinitely.
    maxTimeMS: 60 * 1000,
}));
assert.eq(testDB[collName].find(updateDoc).toArray().length, 1);

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));
sleepCommand();
})();
