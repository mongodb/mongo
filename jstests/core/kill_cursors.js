// Test the killCursors command.
(function() {
    'use strict';

    var cmdRes;
    var cursor;
    var cursorId;

    var coll = db.jstest_killcursors;
    coll.drop();

    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // killCursors command should fail if the collection name is not a string.
    cmdRes = db.runCommand({
        killCursors: {foo: "bad collection param"},
        cursors: [NumberLong(123), NumberLong(456)]
    });
    assert.commandFailedWithCode(cmdRes, ErrorCodes.FailedToParse);

    // killCursors command should fail if the cursors parameter is not an array.
    cmdRes = db.runCommand({
        killCursors: coll.getName(),
        cursors: {a: NumberLong(123), b: NumberLong(456)}
    });
    assert.commandFailedWithCode(cmdRes, ErrorCodes.FailedToParse);

    // killCursors command should fail if the cursors parameter is an empty array.
    cmdRes = db.runCommand({
        killCursors: coll.getName(),
        cursors: []
    });
    assert.commandFailedWithCode(cmdRes, ErrorCodes.BadValue);

    // killCursors command should report cursors as not found if the collection does not exist.
    cmdRes = db.runCommand({
        killCursors: "non-existent-collection",
        cursors: [NumberLong(123), NumberLong(456)]
    });
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursorsKilled, []);
    assert.eq(cmdRes.cursorsNotFound, [NumberLong(123), NumberLong(456)]);
    assert.eq(cmdRes.cursorsAlive, []);
    assert.eq(cmdRes.cursorsUnknown, []);

    // killCursors command should report non-existent cursors as "not found".
    cmdRes = db.runCommand({
        killCursors: coll.getName(),
        cursors: [NumberLong(123), NumberLong(456)]
    });
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursorsKilled, []);
    assert.eq(cmdRes.cursorsNotFound, [NumberLong(123), NumberLong(456)]);
    assert.eq(cmdRes.cursorsAlive, []);
    assert.eq(cmdRes.cursorsUnknown, []);

    // Test a case where one cursors exists and is killed but the other does not exist.
    cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
    assert.commandWorked(cmdRes);
    cursorId = cmdRes.cursor.id;
    assert.neq(cursorId, NumberLong(0));

    cmdRes = db.runCommand({
        killCursors: coll.getName(),
        cursors: [NumberLong(123), cursorId]
    });
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursorsKilled, [cursorId]);
    assert.eq(cmdRes.cursorsNotFound, [NumberLong(123)]);
    assert.eq(cmdRes.cursorsAlive, []);
    assert.eq(cmdRes.cursorsUnknown, []);

    // Test killing a noTimeout cursor.
    cmdRes = db.runCommand({find: coll.getName(), batchSize: 2, noCursorTimeout: true});
    assert.commandWorked(cmdRes);
    cursorId = cmdRes.cursor.id;
    assert.neq(cursorId, NumberLong(0));

    cmdRes = db.runCommand({
        killCursors: coll.getName(),
        cursors: [NumberLong(123), cursorId]
    });
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursorsKilled, [cursorId]);
    assert.eq(cmdRes.cursorsNotFound, [NumberLong(123)]);
    assert.eq(cmdRes.cursorsAlive, []);
    assert.eq(cmdRes.cursorsUnknown, []);

    // Test killing a pinned cursor. Since cursors are generally pinned for short periods of time
    // while result batches are generated, this requires some special machinery to keep a cursor
    // permanently pinned.
    var failpointName = "keepCursorPinnedDuringGetMore";
    var cleanup;
    try {
        // Enable a failpoint to ensure that the cursor remains pinned.
        assert.commandWorked(db.adminCommand({
            configureFailPoint: failpointName,
            mode: "alwaysOn"
        }));

        cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
        assert.commandWorked(cmdRes);
        cursorId = cmdRes.cursor.id;
        assert.neq(cursorId, NumberLong(0));

        // Pin the cursor during a getMore, and wait to make sure it has been pinned.
        var numPinnedBefore = db.serverStatus().metrics.cursor.open.pinned;
        var code = 'db.runCommand({getMore: ' + cursorId.toString() +
                   ', collection: "' + coll.getName() + '"});'
        cleanup = startParallelShell(code);
        assert.soon(function() {
            // It's possible that the internally issued queries (e.g. syncing from primary to
            // secondary in a replica set) can cause an additional cursor to be pinned. This is why
            // we check "> numPinnedBefore" rather than "== numPinnedBefore + 1".
            return (db.serverStatus().metrics.cursor.open.pinned > numPinnedBefore);
        }, "Failed to pin cursor. Cursors pinned before running getMore: " + numPinnedBefore);

        cmdRes = db.runCommand({isMaster: 1});
        assert.commandWorked(cmdRes);
        var isMongos = (cmdRes.msg === "isdbgrid");

        // Attempt to kill a pinned cursor. Currently pinned cursors that are targeted by a
        // killCursors operation are kept alive on mongod but are killed on mongos (see
        // SERVER-21710).
        cmdRes = db.runCommand({
            killCursors: coll.getName(),
            cursors: [NumberLong(123), cursorId]
        });
        assert.commandWorked(cmdRes);
        assert.eq(cmdRes.cursorsKilled, isMongos ? [cursorId] : []);
        assert.eq(cmdRes.cursorsNotFound, [NumberLong(123)]);
        assert.eq(cmdRes.cursorsAlive, isMongos ? [] : [cursorId]);
        assert.eq(cmdRes.cursorsUnknown, []);
    } finally {
        assert.commandWorked(db.adminCommand({configureFailPoint: failpointName, mode: "off"}));
        cleanup();
    }
})();
