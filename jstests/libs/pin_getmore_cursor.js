/**
 * Pins a cursor in a seperate shell and then runs the given function.
 * 'conn': a connection to an instance of a mongod or mongos.
 * 'assertFunction': a function containing the test to be run after a cursor is pinned and hanging.
 * 'runGetMoreFunc': A function to generate a string that will be executed in the parallel shell.
 * 'failPointName': The string name of the failpoint where the cursor will hang. The function turns
 * the failpoint on, the assert function should turn it off whenever it is appropriate for the test.
 */

function withPinnedCursor({conn, assertFunction, runGetMoreFunc, failPointName}) {
    // This test runs manual getMores using different connections, which will not inherit the
    // implicit session of the cursor establishing command.
    TestData.disableImplicitSessions = true;

    const db = conn.getDB("test");
    const coll = db.jstest_with_pinned_cursor;
    coll.drop();
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
        let cursorId = cmdRes.cursor.id;
        assert.neq(cursorId, NumberLong(0));
        // Let the cursor hang in a different shell
        let code = "let cursorId = " + cursorId.toString() + ";";
        code += "let collName = '" + coll.getName() + "';";
        code += "(" + runGetMoreFunc.toString() + ")();";
        cleanup = startParallelShell(code, conn.port);
        // Wait until we know the failpoint has been reached.
        assert.soon(function() {
            const arr =
                db.getSiblingDB("admin")
                    .aggregate([{$currentOp: {localOps: true}}, {$match: {"msg": failPointName}}])
                    .toArray();
            return arr.length > 0;
        });
        assertFunction(cursorId, coll);
        // Eventually the cursor should be cleaned up.
        assert.commandWorked(db.adminCommand({configureFailPoint: failPointName, mode: "off"}));
        assert.soon(() => db.serverStatus().metrics.cursor.open.pinned == 0);

        // Trying to kill the cursor again should result in the cursor not being found.
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
