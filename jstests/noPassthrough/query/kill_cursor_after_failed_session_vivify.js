/**
 * Regression test for a use-after-free caused by a cursor left pinned to a destroyed
 * OperationContext.
 *
 * All three tests pass once the cursor's pin state is reverted on the vivify-failure path.
 *
 * @tags: [requires_sharding_disabled]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

// Manage every logical session slot explicitly so the cache fills predictably.
TestData.disableImplicitSessions = true;

const maxSessions = 5;
const collName = "foo";

// Leaving one cursor pinned to a destroyed OperationContext.
// Returns the handles the caller needs for the effect under test.
function leakVictimCursor(conn) {
    const testDB = conn.getDB("test");
    assert.commandWorked(testDB[collName].insert([{x: 0}, {x: 1}, {x: 2}]));

    // Open a cursor under an explicit "victim" session, leaving it open on the server.
    const victimSession = conn.startSession();
    const victimDB = victimSession.getDatabase("test");
    const findRes = assert.commandWorked(victimDB.runCommand({find: collName, batchSize: 1}));
    const cursorId = findRes.cursor.id;
    assert.neq(cursorId, NumberLong(0), "cursor should remain open on the server");
    const victimLsid = victimSession.getSessionId();

    // Block every getMore before its session is vivified into the cache.
    const fp = configureFailPoint(conn, "failCommand", {
        failCommands: ["getMore"],
        blockConnection: true,
    });

    // Send the getMore from a parallel shell; it will block inside the failpoint and,
    // once released, fail because the session cache is full.
    const awaitGetMore = startParallelShell(
        funWithArgs(
            function (port, collName, cursorId, victimLsid) {
                const db = new Mongo("localhost:" + port).getDB("test");
                const res = db.runCommand({getMore: cursorId, collection: collName, lsid: victimLsid});
                assert.commandFailedWithCode(res, ErrorCodes.TooManyLogicalSessions);
            },
            conn.port,
            collName,
            cursorId,
            victimLsid,
        ),
        conn.port,
    );

    // Wait until the getMore is parked inside the failpoint.
    fp.wait();

    // Evict all active sessions, then fill the cache to capacity with other sessions.
    assert.commandWorked(conn.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

    const fillerSessions = [];
    for (let i = 0; i < maxSessions; i++) {
        const s = conn.startSession();
        assert.commandWorked(s.getDatabase("test").runCommand({ping: 1}));
        fillerSessions.push(s);
    }

    // Release the getMore. Its session can no longer be vivified -> TooManyLogicalSessions
    // is raised while the cursor is pinned to the getMore's OperationContext.
    fp.off();
    awaitGetMore();

    return {cursorId, victimSession, fillerSessions};
}

describe("cursor left pinned after a failed session vivify", function () {
    let conn;

    afterEach(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
            conn = null;
        }
    });

    it("survives a killCursors on the affected cursor", function () {
        conn = MongoRunner.runMongod({setParameter: {maxSessions}});
        const {cursorId, victimSession, fillerSessions} = leakVictimCursor(conn);

        // killCursors must not dereference the (now destroyed) getMore OperationContext.
        // Use a session that is already cached so killCursors itself does not fail to vivify.
        assert.commandWorked(
            fillerSessions[0].getDatabase("test").runCommand({killCursors: collName, cursors: [cursorId]}),
        );

        // The server must still be alive and responsive
        assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));

        for (const s of fillerSessions) {
            s.endSession();
        }
        victimSession.endSession();
    });

    it("times out the leaked cursor", function () {
        // Keep a long cursor timeout during setup so the (idle, not-yet-pinned) victim cursor
        // isn't reaped while the getMore is parked in the failpoint and we fill the cache.
        conn = MongoRunner.runMongod({
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
        // '_operationUsingCursor' keeps cursorShouldTimeout_inlock from ever
        // reaping it.
        assert.soon(
            () => conn.getDB("admin").serverStatus().metrics.cursor.timedOut >= 1,
            "leaked cursor was never timed out",
        );

        for (const s of fillerSessions) {
            s.endSession();
        }
        victimSession.endSession();
    });

    it("shuts down cleanly without tripping the pin invariant", function () {
        conn = MongoRunner.runMongod({setParameter: {maxSessions}});
        const {victimSession, fillerSessions} = leakVictimCursor(conn);

        for (const s of fillerSessions) {
            s.endSession();
        }
        victimSession.endSession();

        // A cursor still pinned to a destroyed OperationContext trips the invariant in
        // ~CursorManager at shutdown. stopMongod asserts a clean exit, so a non-clean exit
        // fails this test.
        MongoRunner.stopMongod(conn);
        conn = null;
    });
});
