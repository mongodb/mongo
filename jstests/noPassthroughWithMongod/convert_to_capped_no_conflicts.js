/**
 * Tests that convertToCapped does not conflict with a database MODE_IX lock.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");

const collName = "convert_to_capped_no_conflicts";
const testDB = db.getSiblingDB("test");
testDB.dropDatabase();
const testColl = testDB.getCollection(collName);

const sleepFunction = function(sleepDB) {
    // If convertToCapped calls need to wait on this lock, holding this lock for 4 hours will
    // trigger a test timeout.
    assert.commandFailedWithCode(
        db.getSiblingDB("test").adminCommand(
            {sleep: 1, secs: 18000, lockTarget: sleepDB, lock: "iw", $comment: "Lock sleep"}),
        ErrorCodes.Interrupted);
};

const sleepCommand = startParallelShell(funWithArgs(sleepFunction, "test"), testDB.getMongo().port);
const sleepID =
    waitForCommand("sleepCmd",
                   op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"),
                   testDB.getSiblingDB("admin"));

assert.commandWorked(testColl.insert({a: 1}));
assert(!testColl.isCapped());
assert.commandWorked(testDB.runCommand({convertToCapped: collName, size: 1}));
assert(testColl.isCapped());

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));
sleepCommand();
})();
