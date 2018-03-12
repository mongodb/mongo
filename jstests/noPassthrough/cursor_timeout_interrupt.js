// Tests the cursor timeout background thread is not subject to interrupt when acquiring locks. This
// test was designed to reproduce SERVER-33785.
(function() {
    "use strict";

    // We're testing the client cursor monitor thread, so make it run with a higher frequency.
    const options = {setParameter: {clientCursorMonitorFrequencySecs: 0}};
    const conn = MongoRunner.runMongod(options);
    assert.neq(conn, null, `Mongod failed to start up with options ${tojson(options)}`);
    const db = conn.getDB("test");
    const coll = db.cursor_timeout_interrupt;

    // Create a collection and open a cursor on the collection.
    const batchSize = 10;
    for (var i = 1; i < 2 * batchSize; ++i) {
        assert.writeOK(coll.insert({}));
    }
    const cursor = coll.find({}).batchSize(batchSize);
    cursor.next();
    assert.eq(db.serverStatus().metrics.cursor.open.total, 1);
    const originalTopTotal = db.adminCommand({top: 1}).totals[coll.getFullName()].total;

    // Start an operation which is holding the lock the timeout thread will need. At a minimum, we
    // need someone to be holding the lock in order for the thread to ever bother checking for
    // interrupt when acquiring locks.
    const awaitConflictingLock = startParallelShell(
        () => assert.commandFailedWithCode(db.adminCommand({sleep: 1, lock: "w", secs: 5 * 60}),
                                           ErrorCodes.Interrupted),
        conn.port);

    // Enable a failpoint that will cause a thread to always be interrupted. Test that the client
    // cursor monitor thread ignores interrupts, including during lock acquisitions.
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "checkForInterruptFail",
        mode: "alwaysOn",
        data: {threadName: "clientcursormon", chance: 1}
    }));

    // Lower the cursor timeout threshold to make the newly created cursor eligible for timeout.
    assert.commandWorked(db.adminCommand({setParameter: 1, cursorTimeoutMillis: 1}));

    // Wait to verify that the client cursor is blocked waiting on the thread holding the lock.
    assert.soon(() => db.currentOp({desc: "clientcursormon", active: true, waitingForLock: true})
                          .inprog.length === 1);

    const sleepOps = db.getSiblingDB("admin").currentOp({"command.sleep": 1, active: true}).inprog;
    assert.eq(sleepOps.length, 1);
    const sleepOpId = sleepOps[0].opid;
    assert.commandWorked(db.adminCommand({killOp: 1, op: sleepOpId}));

    assert.soon(() => db.serverStatus().metrics.cursor.open.total == 0);
    assert.eq(db.serverStatus().metrics.cursor.timedOut, 1);
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "checkForInterruptFail", mode: "off"}));

    // Test that the client cursor monitor does not increment any stats for top.
    assert.eq(db.adminCommand({top: 1}).totals[coll.getFullName()].total, originalTopTotal);
    awaitConflictingLock();

    MongoRunner.stopMongod(conn);
}());
