(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/wait_for_command.js");
const dbName = "test";
const collName = "create_new_index_no_conflicts";
const testDB = db.getSiblingDB(dbName);
testDB.dropDatabase();
const testColl = testDB.getCollection(collName);

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

// This command will implicitly create `testColl`.
let res = assert.commandWorked(testColl.createIndex({a: 1}));
assert.eq(1, res.numIndexesBefore);
assert.eq(2, res.numIndexesAfter);
assert(res.createdCollectionAutomatically);

// Interrupt the sleep command.
assert.commandWorked(testDB.getSiblingDB("admin").killOp(sleepID));

sleepCommand();

// Since `testColl` has been created, the following will create an index on an existing collection.
res = assert.commandWorked(testColl.createIndex({b: 1}));
assert.eq(2, res.numIndexesBefore);
assert.eq(3, res.numIndexesAfter);
assert(!res.createdCollectionAutomatically);
})();
