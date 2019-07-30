/**
 * Tests that checking out an already checked out session doesn't lead to a self-deadlock. This is a
 * regression test for SERVER-36007.
 *
 * @tags: [uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");

function doInsertWithSession(host, lsid, txnNumber) {
    try {
        const conn = new Mongo(host);
        const db = conn.getDB("test");
        assert.commandWorked(db.runCommand({
            insert: "mycoll",
            documents: [{_id: txnNumber}],
            lsid: {id: eval(lsid)},
            txnNumber: NumberLong(txnNumber),
        }));
        return {ok: 1};
    } catch (e) {
        print("doInsertWithSession failed with " + e.toString());
        return {ok: 0, error: e.toString(), stack: e.stack};
    }
}

let thread1;
let thread2;

// We fsyncLock the server so that a transaction operation will block waiting for a lock.
assert.commandWorked(db.fsyncLock());
try {
    // JavaScript objects backed by C++ objects (e.g. BSON values) do not serialize correctly
    // when passed through the Thread constructor. To work around this behavior, we
    // instead pass a stringified form of the JavaScript object through the Thread
    // constructor and use eval() to rehydrate it.
    const lsid = UUID();
    thread1 = new Thread(doInsertWithSession, primary.host, tojson(lsid), 1);
    thread1.start();

    assert.soon(
        () => {
            const ops = db.currentOp(
                {"command.insert": "mycoll", "command.txnNumber": {$eq: 1}, waitingForLock: true});
            return ops.inprog.length === 1;
        },
        () => {
            return "insert operation with txnNumber 1 was not found: " + tojson(db.currentOp());
        });

    thread2 = new Thread(doInsertWithSession, primary.host, tojson(lsid), 2);
    thread2.start();

    // Run currentOp() again to ensure that thread2 has started its insert command.
    assert.soon(
        () => {
            const ops = db.currentOp({"command.insert": "mycoll", "command.txnNumber": {$eq: 2}});
            return ops.inprog.length === 1;
        },
        () => {
            return "insert operation with txnNumber 2 was not found: " + tojson(db.currentOp());
        });
} finally {
    // We run the fsyncUnlock command in a finally block to avoid leaving the server fsyncLock'd
    // if the test were to fail.
    assert.commandWorked(db.fsyncUnlock());
}

thread1.join();
thread2.join();

assert.commandWorked(thread1.returnData());
assert.commandWorked(thread2.returnData());

rst.stopSet();
})();
