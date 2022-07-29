// Test that the dataSize command can be interrupted. Failpoint is defined for mongod only,
// therefore this test is running only in unsharded setup.

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");         // For configureFailPoint.
load("jstests/libs/wait_for_command.js");        // For waitForCommand.
load("jstests/libs/parallel_shell_helpers.js");  // For funWithArgs.

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);
assert.neq(null, conn, `mongod failed to start with options ${tojson(mongodOptions)}`);

const dbName = `${jsTest.name()}_db`;
const db = conn.getDB(dbName);
assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.createCollection("foo"));

const coll = db.foo;
coll.drop();
coll.insert({_id: 0, s: "asdasdasdasdasdasdasd"});

const dataSizeCommand = {
    "dataSize": dbName + ".foo",
    "keyPattern": {"_id": 1},
    "min": {"_id": 0},
    "max": {"_id": 1}
};

// Set the yield iterations to 1 such that on every getNext() call we check for yield or interrupt.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

// Configure the failpoint.
const failpoint = configureFailPoint(db, "hangBeforeDatasizeCount");

// Launch a parallel shell that runs the dataSize command, that should fail due to interrupt.
const awaitShell =
    startParallelShell(funWithArgs(function(cmd) {
                           assert.commandFailedWithCode(db.runCommand(cmd), ErrorCodes.Interrupted);
                       }, dataSizeCommand), db.getMongo().port);
failpoint.wait();

// Find the command opid and kill it.
const opId =
    waitForCommand("dataSizeCmd", op => (op["command"]["dataSize"] == dbName + ".foo"), db);
assert.commandWorked(db.killOp(opId));

// The command is not killed just yet. It will be killed, after releasing the failpoint.
assert.neq(waitForCommand("dataSizeCmd", op => (op["command"]["dataSize"] == dbName + ".foo"), db),
           -1);

failpoint.off();
awaitShell();

assert.commandWorked(db.dropDatabase());
MongoRunner.stopMongod(conn);
})();
