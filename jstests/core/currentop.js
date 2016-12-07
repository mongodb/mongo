/**
 * Tests that long-running operations show up in currentOp and report the locks they are holding.
 */
(function() {
    "use strict";
    const coll = db.jstests_currentop;
    coll.drop();

    // We fsync+lock the server to cause all subsequent write operations to block.
    assert.commandWorked(db.fsyncLock());

    const awaitInsertShell = startParallelShell(function() {
        assert.writeOK(db.jstests_currentop.insert({}));
    });

    // Wait until the write appears in the currentOp output reporting that it is waiting for a lock.
    assert.soon(
        function() {
            return db.currentOp({
                         ns: coll.getFullName(),
                         "locks.Global": "w",
                         "waitingForLock": true,
                     }).inprog.length === 1;
        },
        function() {
            return "Failed to find blocked insert in currentOp() output: " + tojson(db.currentOp());
        });

    // Unlock the server and make sure the write finishes.
    const fsyncResponse = assert.commandWorked(db.fsyncUnlock());
    assert.eq(fsyncResponse.lockCount, 0);
    awaitInsertShell();
}());
