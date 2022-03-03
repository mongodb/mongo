// Test setUserWriteBlockMode command.
//
// @tags: [
//   requires_fcv_53,
//   requires_non_retryable_commands,
//   requires_replication,
// ]

(function() {
'use strict';

const runTest = (frontend, backend) => {
    const db = frontend.getDB(jsTestName());
    const coll = db.test;
    const admin = backend.getDB("admin");

    assert.commandWorked(coll.insert({a: 2}));

    // With setUserWriteBlockMode enabled, ensure that inserts, updates, and removes fail and don't
    // modify data
    assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: true}));
    assert.commandFailedWithCode(coll.insert({a: 1}), ErrorCodes.OperationFailed);
    assert.commandFailedWithCode(coll.update({a: 2}, {a: 2, b: 2}), ErrorCodes.OperationFailed);
    assert.commandFailedWithCode(coll.remove({a: 2}), ErrorCodes.OperationFailed);
    assert.eq(1, coll.find({a: 2}).count());
    assert.eq(0, coll.find({a: 1}).count());
    assert.eq(0, coll.find({b: 2}).count());

    // Disable userWriteBlockMode and ensure that the above operations now succeed and modify data
    assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: false}));
    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.update({a: 2}, {a: 2, b: 2}));
    assert.eq(1, coll.find({a: 2, b: 2}).count());
    assert.commandWorked(coll.remove({a: 2}));
    assert.eq(0, coll.find({a: 2}).count());
    assert.eq(1, coll.find({a: 1}).count());
};

// Test on standalone
const conn = MongoRunner.runMongod();
runTest(conn, conn);
MongoRunner.stopMongod(conn);

// Test on replset primary
const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
runTest(primary, primary);
rst.stopSet();
})();
