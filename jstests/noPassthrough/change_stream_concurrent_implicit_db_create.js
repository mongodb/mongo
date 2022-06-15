// Tests that concurrent change streams requests that would create the database will take locks in
// an order that avoids a deadlock.
// This test was designed to reproduce SERVER-34333.
// This test uses the WiredTiger storage engine, which does not support running without journaling.
// @tags: [requires_replication, requires_majority_read_concern]
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const db = rst.getPrimary().getDB("test");

let unique_dbName = jsTestName();
const sleepShell = startParallelShell(() => {
    assert.commandFailedWithCode(db.adminCommand({sleep: 1, lock: "w", seconds: 600}),
                                 ErrorCodes.Interrupted);
}, rst.getPrimary().port);
assert.soon(
    () =>
        db.getSiblingDB("admin").currentOp({"command.sleep": 1, active: true}).inprog.length === 1);
const sleepOps = db.getSiblingDB("admin").currentOp({"command.sleep": 1, active: true}).inprog;
assert.eq(sleepOps.length, 1);
const sleepOpId = sleepOps[0].opid;

// Start two concurrent shells which will both attempt to create the database which does not yet
// exist.
const openChangeStreamCode = `const cursor = db.getSiblingDB("${unique_dbName}").test.watch();`;
const changeStreamShell1 = startParallelShell(openChangeStreamCode, rst.getPrimary().port);
const changeStreamShell2 = startParallelShell(openChangeStreamCode, rst.getPrimary().port);

// Wait until we can see both change streams have started and are waiting to acquire the lock
// held by the sleep command.
assert.soon(
    () => db.currentOp({"command.aggregate": "test", waitingForLock: true}).inprog.length === 2);
assert.commandWorked(db.adminCommand({killOp: 1, op: sleepOpId}));

sleepShell();

// Before the fix for SERVER-34333, the operations in these shells would be deadlocked with each
// other and never complete.
changeStreamShell1();
changeStreamShell2();

rst.stopSet();
}());
