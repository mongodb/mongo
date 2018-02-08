// @tags: [requires_getmore, does_not_support_stepdowns]
//
// Uses getMore to pin an open cursor.
//
// Does not support stepdowns because if a stepdown were to occur between running find() and
// calling killCursors on the cursor ID returned by find(), the killCursors might be sent to
// different node than the one which has the cursor. This would result in the node returning
// "CursorNotFound."
//
// Test killing a pinned cursor. Since cursors are generally pinned for short periods while result
// batches are generated, this requires some special machinery to keep a cursor permanently pinned.

(function() {
    "use strict";

    const kFailPointName = "keepCursorPinnedDuringGetMore";

    var conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod failed to start up");
    const db = conn.getDB("test");

    let coll = db.jstest_kill_pinned_cursor;
    coll.drop();

    for (let i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    let cleanup = null;
    let cursorId;

    // kill the cursor associated with the command and assert that we get the
    // OperationInterrupted error.
    try {
        assert.commandWorked(
            db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

        let cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
        assert.commandWorked(cmdRes);
        cursorId = cmdRes.cursor.id;
        assert.neq(cursorId, NumberLong(0));

        let runGetMoreAndExpectError = function() {
            let response = db.runCommand({getMore: cursorId, collection: collName});
            // We expect that the operation will get interrupted and fail.
            assert.commandFailedWithCode(response, ErrorCodes.CursorKilled);
        };
        let code = "let cursorId = " + cursorId.toString() + ";";
        code += "let collName = '" + coll.getName() + "';";
        code += "(" + runGetMoreAndExpectError.toString() + ")();";
        cleanup = startParallelShell(code, conn.port);

        // Sleep until we know the cursor is pinned.
        assert.soon(() => db.serverStatus().metrics.cursor.open.pinned > 0);

        cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
        assert.commandWorked(cmdRes);
        assert.eq(cmdRes.cursorsKilled, [cursorId]);
        assert.eq(cmdRes.cursorsAlive, []);
        assert.eq(cmdRes.cursorsNotFound, []);
        assert.eq(cmdRes.cursorsUnknown, []);
    } finally {
        assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));
        if (cleanup) {
            cleanup();
        }
    }

    // Eventually the cursor should be cleaned up.
    assert.soon(() => db.serverStatus().metrics.cursor.open.pinned == 0);

    // Trying to kill the cursor again should result in the cursor not being found.
    let cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursorsKilled, []);
    assert.eq(cmdRes.cursorsAlive, []);
    assert.eq(cmdRes.cursorsNotFound, [cursorId]);
    assert.eq(cmdRes.cursorsUnknown, []);
    MongoRunner.stopMongod(conn);
})();
