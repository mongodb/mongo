/**
 * Creates a cursor, then pins it in a parallel shell by using the provided 'failPointName' and
 * 'runGetMoreFunc'. Runs 'assertFunction' while the cursor is pinned, then unpins it.
 *
 * 'conn': a connection to an instance of a mongod or mongos.
 * 'sessionId': The id present if the database is currently in a session.
 * 'dbName': the database to use with the cursor.
 *
 * 'assertFunction(cursorId, coll)':
 *   A function containing the test to be run while the cursor is pinned.
 *
 * 'runGetMoreFunc':
 *   A function to be executed in the parallel shell. It is expected to hit the fail point, defined
 *   in 'failPointName' by calling 'db.runCommand({getMore: cursorId, collection: collName})' but
 *   it can do additional validation on the result of the command or run other commands.
 *
 * 'failPointName': name of the failpoint where 'runGetMoreFunc' is expected to hang.
 *
 * 'assertEndCounts': whether to assert zero pinned cursors an the end.
 */

load("jstests/libs/curop_helpers.js");  // For waitForCurOpByFailPoint().
load('jstests/libs/parallel_shell_helpers.js');

function withPinnedCursor(
    {conn, sessionId, db, assertFunction, runGetMoreFunc, failPointName, assertEndCounts}) {
    // This test runs manual getMores using different connections, which will not inherit the
    // implicit session of the cursor establishing command.
    TestData.disableImplicitSessions = true;

    const coll = db.jstest_with_pinned_cursor;
    coll.drop();
    db.active_cursor_sentinel.drop();
    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(coll.insert({value: i}));
    }
    let cleanup = null;
    try {
        // Issue an initial find in order to create a cursor and obtain its cursorID.
        let cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
        assert.commandWorked(cmdRes);
        const cursorId = cmdRes.cursor.id;
        assert.neq(cursorId, NumberLong(0));

        // Enable the specified failpoint.
        assert.commandWorked(
            db.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));

        // In a different shell pin the cursor by calling 'getMore' on it that would be blocked by
        // the failpoint.
        cleanup =
            startParallelShell(funWithArgs(function(runGetMoreFunc, collName, cursorId, sessionId) {
                                   runGetMoreFunc(collName, cursorId, sessionId);
                                   db.active_cursor_sentinel.insert({});
                               }, runGetMoreFunc, coll.getName(), cursorId, sessionId), conn.port);

        // Wait until we know the failpoint has been reached.
        waitForCurOpByFailPointNoNS(db, failPointName, {}, {localOps: true, allUsers: true});

        // The assert function might initiate killing of the cursor. Because the cursor is pinned,
        // it actually won't be killed until the pin is removed but it will interrupt 'getMore' in
        // the parallel shell after the failpoint is unset.
        assertFunction(cursorId, coll);

        // Unsetting the failpoint allows getMore in the parallel shell to proceed and unpins the
        // cursor, which will either exhaust or detect interrupt (if 'assertFunction' killed the
        // cursor).
        assert.commandWorked(db.adminCommand({configureFailPoint: failPointName, mode: "off"}));

        // Wait for the parallel shell to be done with 'getMore' command. We'd know when it moves on
        // to inserting the sentinel object.
        assert.soon(() => db.active_cursor_sentinel.find().itcount() > 0);

        // Give the server up to 5 sec to dispose of the cursor.
        if (assertEndCounts) {
            assert.retry(
                () => {
                    return db.serverStatus().metrics.cursor.open.pinned == 0;
                },
                "Expected 0 pinned cursors, but have " + tojson(db.serverStatus().metrics.cursor),
                10 /* num_attempts */,
                500 /* intervalMS */);
        }

        // By now either getMore in the parallel shell has exhausted the cursor, or the cursor has
        // been killed by 'assertFunction'. In both cases, an attempt to kill the cursor again
        // should report it as not found.
        cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
        assert.commandWorked(cmdRes);
        assert.eq(cmdRes.cursorsKilled, []);
        assert.eq(cmdRes.cursorsAlive, []);
        assert.eq(cmdRes.cursorsNotFound, [cursorId]);
        assert.eq(cmdRes.cursorsUnknown, []);
    } finally {
        if (cleanup) {
            cleanup();
        }
    }
}
