/**
 * Tests that collMod does not conflict with a database MODE_IX lock.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");

const collName = "collMod_no_conflicts";
const viewName = "testView";
const testDB = db.getSiblingDB("test");
testDB.dropDatabase();

const sleepFunction = function(sleepDB) {
    // If collMod calls need to wait on this lock, holding this lock for 4 hours will
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

assert.commandWorked(testDB.createView(viewName, collName, [{$match: {a: 1}}]));
const collModPipeline = [{$match: {a: 2}}];
assert.commandWorked(
    testDB.runCommand({collMod: viewName, viewOn: collName, pipeline: collModPipeline}));

const res = db.getCollectionInfos({name: viewName});
assert.eq(res.length, 1);
assert.eq(res[0].options.pipeline, collModPipeline);

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));
sleepCommand();
})();
