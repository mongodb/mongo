/**
 * Regression test for a use-after-free caused by a cursor left pinned to a destroyed
 * OperationContext.
 *
 * All three scenarios pass once the cursor's pin state is only attributed after vivify succeeds.
 *
 * @tags: [requires_sharding_disabled]
 */
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

// Manage every logical session slot explicitly so the cache fills predictably.
TestData.disableImplicitSessions = true;

const maxSessions = 5;
const collName = "foo";

// Leaving one cursor pinned to a destroyed
// OperationContext. Returns the handles the caller needs for the effect under test.
function leakVictimCursor(conn) {
    const testDB = conn.getDB("test");
    assert.commandWorked(testDB[collName].insert([{x: 0}, {x: 1}, {x: 2}]));

    // Open a cursor under an explicit "victim" session, leaving it open on the server. Running the
    // find vivifies the victim session into the in-memory session cache.
    const victimSession = conn.startSession();
    const victimDB = victimSession.getDatabase("test");
    const findRes = assert.commandWorked(victimDB.runCommand({find: collName, batchSize: 1}));
    const cursorId = findRes.cursor.id;
    assert.neq(cursorId, NumberLong(0), "cursor should remain open on the server");
    const victimLsid = victimSession.getSessionId();

    // Block getMore for a fixed window. On this branch 'failCommand' requires 'blockTimeMS' when
    // 'blockConnection' is true, so we block long enough to evict+fill the cache before it resumes.
    const blockMillis = 10000;
    const fp = configureFailPoint(conn, "failCommand", {
        failCommands: ["getMore"],
        blockConnection: true,
        blockTimeMS: blockMillis,
    });

    // Send the getMore from a parallel shell; it vivifies the victim session at dispatch (cache
    // still has room), then blocks before pinCursor. When it resumes the cache is full, so pinning
    // fails to vivify with TooManyLogicalSessions.
    const awaitGetMore = startParallelShell(
        funWithArgs(function(port, collName, cursorId, victimLsid) {
            const db = new Mongo("localhost:" + port).getDB("test");
            const res = db.runCommand({getMore: cursorId, collection: collName, lsid: victimLsid});
            assert.commandFailedWithCode(res, ErrorCodes.TooManyLogicalSessions);
        }, conn.port, collName, cursorId, victimLsid), conn.port);

    // Wait until the getMore is parked inside the failpoint (its dispatch-time vivify has run).
    fp.wait();

    // Flush the session cache (evicts the victim session), then fill it to capacity with others.
    assert.commandWorked(conn.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    const fillerSessions = [];
    for (let i = 0; i < maxSessions; i++) {
        const s = conn.startSession();
        assert.commandWorked(s.getDatabase("test").runCommand({ping: 1}));
        fillerSessions.push(s);
    }

    // Let the block expire and the getMore finish (failing inside pinCursor).
    awaitGetMore();

    return {cursorId, victimSession, fillerSessions};
}

// Scenario 1: killCursors must not dereference the (now destroyed) getMore OperationContext.
(function survivesKillCursors() {
    const conn = MongoRunner.runMongod({setParameter: {maxSessions}});
    const {cursorId, victimSession, fillerSessions} = leakVictimCursor(conn);

    // Use a session that is already cached so killCursors itself does not fail to vivify.
    assert.commandWorked(fillerSessions[0].getDatabase("test").runCommand(
        {killCursors: collName, cursors: [cursorId]}));

    // The server must still be alive and responsive (i.e. no use-after-free crash).
    assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));

    for (const s of fillerSessions) {
        s.endSession();
    }
    victimSession.endSession();
    MongoRunner.stopMongod(conn);
})();

// Scenario 2: the leaked cursor must become eligible for timeout once unpinned.
(function timesOutLeakedCursor() {
    // Keep a long cursor timeout during setup so the idle victim cursor isn't reaped before we
    // observe it; drop it to near-zero afterwards.
    const conn = MongoRunner.runMongod({
        setParameter: {
            maxSessions,
            enableTimeoutOfInactiveSessionCursors: true,
            clientCursorMonitorFrequencySecs: 1,
            cursorTimeoutMillis: 600000,
        },
    });
    const {victimSession, fillerSessions} = leakVictimCursor(conn);

    // Now make idle cursors eligible for timeout almost immediately.
    assert.commandWorked(conn.getDB("admin").runCommand({setParameter: 1, cursorTimeoutMillis: 1}));

    // With the pin cleared, the idle cursor is eligible for timeout. A non-null
    // '_operationUsingCursor' keeps cursorShouldTimeout_inlock from ever reaping it.
    assert.soon(() => conn.getDB("admin").serverStatus().metrics.cursor.timedOut >= 1,
                "leaked cursor was never timed out");

    for (const s of fillerSessions) {
        s.endSession();
    }
    victimSession.endSession();
    MongoRunner.stopMongod(conn);
})();

// Scenario 3: a cursor still pinned to a destroyed OperationContext trips the invariant in
// ~CursorManager at shutdown. stopMongod asserts a clean exit, so a non-clean exit fails here.
(function shutsDownCleanly() {
    const conn = MongoRunner.runMongod({setParameter: {maxSessions}});
    const {victimSession, fillerSessions} = leakVictimCursor(conn);

    for (const s of fillerSessions) {
        s.endSession();
    }
    victimSession.endSession();

    MongoRunner.stopMongod(conn);
})();
