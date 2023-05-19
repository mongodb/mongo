/**
 * Tests that cloneCollectionAsCapped does not conflict with a database MODE_IX lock.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");

const baseName = "clone_collection_as_capped_no_conflicts";
const fromCollName = baseName + "_from";
const toCollName = baseName + "_to";

const testDB = db.getSiblingDB("test");
testDB.dropDatabase();
const fromColl = testDB.getCollection(fromCollName);
const toColl = testDB.getCollection(toCollName);

const sleepFunction = function(sleepDB) {
    // If cloneCollectionAsCapped calls need to wait on this lock, holding this lock for 4 hours
    // will trigger a test timeout.
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

assert.commandWorked(fromColl.insert({a: 1}));
assert(!fromColl.isCapped());
assert.commandWorked(testDB.runCommand(
    {cloneCollectionAsCapped: fromCollName, toCollection: toCollName, size: 100}));
assert(toColl.isCapped());
assert.eq(toColl.count(), 1);

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));
sleepCommand();
})();
