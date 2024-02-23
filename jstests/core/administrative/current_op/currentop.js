/**
 * Tests that long-running operations show up in currentOp and report the locks they are holding.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: fsyncUnlock.
 *   not_allowed_with_signed_security_token,
 *   # The test expects currentOp to report the insert operation which runs on the primary.
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   uses_parallel_shell,
 * ]
 */

const coll = db.jstests_currentop;
coll.drop();

// We fsync+lock the server to cause all subsequent write operations to block.
assert.commandWorked(db.fsyncLock());

const awaitInsertShell = startParallelShell(function() {
    assert.commandWorked(db.jstests_currentop.insert({}));
});

// Wait until the write appears in the currentOp output reporting that it is waiting for a lock.
assert.soon(
    function() {
        const ops = db.currentOp({
            $and: [
                {"locks.Global": "w", waitingForLock: true},
                // Depending on whether CurOp::setNS_inlock() has been called, the "ns" field
                // may either be the full collection name or the command namespace.
                {
                    $or: [
                        {ns: coll.getFullName()},
                        {ns: db.$cmd.getFullName(), "command.insert": coll.getName()}
                    ]
                },
                {type: "op"}
            ]
        });
        return ops.inprog.length === 1;
    },
    function() {
        return "Failed to find blocked insert in currentOp() output: " + tojson(db.currentOp());
    });

// Unlock the server and make sure the write finishes.
const fsyncResponse = assert.commandWorked(db.fsyncUnlock());
assert.eq(fsyncResponse.lockCount, 0);
awaitInsertShell();
