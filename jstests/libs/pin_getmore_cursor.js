/**
 * Pins a cursor in a seperate shell and then runs the given function.
 * 'conn': a connection to an instance of a mongod or mongos.
 * 'sessionId': The id present if the database is currently in a session.
 * 'dbName': the database to use with the cursor.
 * 'assertFunction': a function containing the test to be run after a cursor is pinned and hanging.
 * 'runGetMoreFunc': A function to generate a string that will be executed in the parallel shell.
 * 'failPointName': The string name of the failpoint where the cursor will hang. The function turns
 * the failpoint on, the assert function should turn it off whenever it is appropriate for the test.
 * 'failPointReachedMessage': The message that should be found in the logs when the failpoint has
 * been reached.
 */

load("jstests/libs/check_log.js");

function withPinnedCursor(
    {conn, sessionId, db, assertFunction, runGetMoreFunc, failPointName, failPointReachedMessage}) {
    // This test runs manual getMores using different connections, which will not inherit the
    // implicit session of the cursor establishing command.
    TestData.disableImplicitSessions = true;

    const coll = db.jstest_with_pinned_cursor;
    coll.drop();
    db.active_cursor_sentinel.drop();
    for (let i = 0; i < 100; ++i) {
        assert.writeOK(coll.insert({value: i}));
    }
    let cleanup = null;
    try {
        // Enable the specified failpoint.
        assert.commandWorked(
            db.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));

        // Issue an initial find in order to create a cursor and obtain its cursorID.
        let cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
        assert.commandWorked(cmdRes);
        const cursorId = cmdRes.cursor.id;
        assert.neq(cursorId, NumberLong(0));

        // Let the cursor hang in a different shell with the information it needs to do a getMore.
        let code = "let cursorId = " + cursorId.toString() + ";";
        code += "let collName = '" + coll.getName() + "';";
        if (sessionId) {
            code += "let sessionId = " + tojson(sessionId) + ";";
        }
        code += "(" + runGetMoreFunc.toString() + ")();";
        code += "db.active_cursor_sentinel.insert({});";

        cleanup = startParallelShell(code, conn.port);

        // Wait until we know the failpoint has been reached.
        checkLog.contains(conn, failPointReachedMessage);
        assertFunction(cursorId, coll);

        // Eventually the cursor should be cleaned up.
        assert.commandWorked(db.adminCommand({configureFailPoint: failPointName, mode: "off"}));

    } finally {
        if (cleanup) {
            cleanup();
        }
    }
}
