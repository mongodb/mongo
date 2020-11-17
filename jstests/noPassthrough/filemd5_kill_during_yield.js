// Tests that interrupting a filemd5 command while the PlanExecutor is yielded will correctly clean
// up the PlanExecutor without crashing the server. This test was designed to reproduce
// SERVER-35361.
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");  // For waitForCurOpByFailPoint().

const conn = MongoRunner.runMongod();
assert.neq(null, conn);
const db = conn.getDB("test");
db.fs.chunks.drop();
assert.commandWorked(db.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "64string")}));
assert.commandWorked(db.fs.chunks.insert({files_id: 1, n: 1, data: new BinData(0, "test")}));
db.fs.chunks.createIndex({files_id: 1, n: 1});

const kFailPointName = "waitInFilemd5DuringManualYield";
assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

const failingMD5Shell =
    startParallelShell(() => assert.commandFailedWithCode(db.runCommand({filemd5: 1, root: "fs"}),
                                                          ErrorCodes.Interrupted),
                       conn.port);

// Wait for filemd5 to manually yield and hang.
const curOps =
    waitForCurOpByFailPoint(db, "test.fs.chunks", kFailPointName, {"command.filemd5": 1});

const opId = curOps[0].opid;

// Kill the operation, then disable the failpoint so the command recognizes it's been killed.
assert.commandWorked(db.killOp(opId));
assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));

failingMD5Shell();
MongoRunner.stopMongod(conn);
}());
