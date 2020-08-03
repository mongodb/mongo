(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");
const dbName = "test";
const collName = "ensure_index_no_conflicts";
const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();
assert.commandWorked(testDB.createCollection(collName));
const testColl = testDB.getCollection(collName);
assert.commandWorked(testColl.createIndex({a: 1}));

let sleepFunction = function(myDbName, myCollName) {
    // If createIndexes calls do need to wait on this lock, holding this lock for 4 hours will
    // trigger a test timeout.
    assert.commandFailedWithCode(db.getSiblingDB("test").adminCommand({
        sleep: 1,
        secs: 18000,
        lockTarget: myDbName + "." + myCollName,
        lock: "iw",
        $comment: "Lock sleep"
    }),
                                 ErrorCodes.Interrupted);
};

let sleepCommand =
    startParallelShell(funWithArgs(sleepFunction, dbName, collName), testDB.getMongo().port);

const sleepID =
    waitForCommand("sleepCmd",
                   op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"),
                   testDB.getSiblingDB("admin"));

assert.commandWorked(testColl.createIndex({a: 1}));
assert.commandFailedWithCode(testColl.createIndex({a: -1}, {name: "a_1"}),
                             ErrorCodes.IndexKeySpecsConflict);

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));

sleepCommand();
})();
