// @tags: [requires_replication, requires_getmore, does_not_support_stepdowns]
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

    // This test runs manual getMores using different connections, which will not inherit the
    // implicit session of the cursor establishing command.
    TestData.disableImplicitSessions = true;

    load("jstests/libs/fixture_helpers.js");  // For "isMongos".

    const st = new ShardingTest({shards: 2});

    // Enables the specified 'failPointName', executes 'runGetMoreFunc' function in a parallel
    // shell, waits for the the failpoint to be hit, then kills the cursor and confirms that the
    // kill was successful.
    function runPinnedCursorKillTest({conn, failPointName, runGetMoreFunc}) {
        const db = conn.getDB("test");
        jsTestLog("Running test with failPoint: " + failPointName);

        const coll = db.jstest_kill_pinned_cursor;
        coll.drop();

        for (let i = 0; i < 10; i++) {
            assert.writeOK(coll.insert({_id: i}));
        }

        let cleanup = null;
        let cursorId;

        try {
            // Enable the specified failpoint.
            assert.commandWorked(
                db.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));

            // Issue an initial find in order to create a cursor and obtain its ID.
            let cmdRes = db.runCommand({find: coll.getName(), batchSize: 2});
            assert.commandWorked(cmdRes);
            cursorId = cmdRes.cursor.id;
            assert.neq(cursorId, NumberLong(0));

            // Serialize 'runGetMoreFunc' along with the cursor ID and collection name, then execute
            // the function in a parallel shell.
            let code = "let cursorId = " + cursorId.toString() + ";";
            code += "let collName = '" + coll.getName() + "';";
            code += "(" + runGetMoreFunc.toString() + ")();";
            cleanup = startParallelShell(code, conn.port);

            // Wait until we know the failpoint has been reached.
            assert.soon(function() {
                const arr =
                    db.getSiblingDB("admin")
                        .aggregate(
                            [{$currentOp: {localOps: true}}, {$match: {"msg": failPointName}}])
                        .toArray();
                return arr.length > 0;
            });

            // Kill the cursor associated with the command and assert that the kill succeeded.
            cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
            assert.commandWorked(cmdRes);
            assert.eq(cmdRes.cursorsKilled, [cursorId]);
            assert.eq(cmdRes.cursorsAlive, []);
            assert.eq(cmdRes.cursorsNotFound, []);
            assert.eq(cmdRes.cursorsUnknown, []);
        } finally {
            assert.commandWorked(db.adminCommand({configureFailPoint: failPointName, mode: "off"}));
            if (cleanup) {
                cleanup();
            }
        }

        // Eventually the cursor should be cleaned up.
        assert.soon(() => db.serverStatus().metrics.cursor.open.pinned == 0);

        // Trying to kill the cursor again should result in the cursor not being found.
        const cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
        assert.commandWorked(cmdRes);
        assert.eq(cmdRes.cursorsKilled, []);
        assert.eq(cmdRes.cursorsAlive, []);
        assert.eq(cmdRes.cursorsNotFound, [cursorId]);
        assert.eq(cmdRes.cursorsUnknown, []);
    }

    // Test that killing the pinned cursor before it starts building the batch results in a
    // CursorKilled exception on a replica set.
    const rs0Conn = st.rs0.getPrimary();
    const testParameters = {
        conn: rs0Conn,
        failPointName: "waitAfterPinningCursorBeforeGetMoreBatch",
        runGetMoreFunc: function() {
            const response = db.runCommand({getMore: cursorId, collection: collName});
            // We expect that the operation will get interrupted and fail.
            assert.commandFailedWithCode(response, ErrorCodes.CursorKilled);
        }
    };
    runPinnedCursorKillTest(testParameters);

    // Check the case where a killCursor is run as we're building a getMore batch on mongod.
    (function() {
        testParameters.conn = rs0Conn;
        testParameters.failPointName = "waitWithPinnedCursorDuringGetMoreBatch";

        // Force yield to occur on every PlanExecutor iteration, so that the getMore is guaranteed
        // to check for interrupts.
        assert.commandWorked(testParameters.conn.getDB("admin").runCommand(
            {setParameter: 1, internalQueryExecYieldIterations: 1}));
        runPinnedCursorKillTest(testParameters);
    })();

    (function() {
        // Run the equivalent test on the mongos. This time, we will force the shards to hang as
        // well. This is so that we can guarantee that the mongos is checking for interruption at
        // the appropriate time, and not just propagating an error it receives from the mongods.
        testParameters.failPointName = "waitAfterPinningCursorBeforeGetMoreBatch";
        FixtureHelpers.runCommandOnEachPrimary({
            db: st.s.getDB("admin"),
            cmdObj: {
                configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch",
                mode: "alwaysOn"
            }
        });
        testParameters.conn = st.s;
        runPinnedCursorKillTest(testParameters);
        FixtureHelpers.runCommandOnEachPrimary({
            db: st.s.getDB("admin"),
            cmdObj: {configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "off"}
        });
    })();

    // Check this case where the interrupt comes in after the batch has been built, and is about to
    // be returned. This is relevant for both mongod and mongos.
    const connsToRunOn = [st.s, rs0Conn];
    for (let conn of connsToRunOn) {
        jsTestLog("Running on conn: " + tojson(conn));

        // Test that, if the pinned cursor is killed after it has finished building a batch, that
        // batch is returned to the client but a subsequent getMore will fail with a
        // 'CursorNotFound' error.
        testParameters.failPointName = "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch";
        testParameters.runGetMoreFunc = function() {
            const getMoreCmd = {getMore: cursorId, collection: collName, batchSize: 2};
            // We expect that the first getMore will succeed, while the second fails because the
            // cursor has been killed.
            assert.commandWorked(db.runCommand(getMoreCmd));
            assert.commandFailedWithCode(db.runCommand(getMoreCmd), ErrorCodes.CursorNotFound);
        };

        runPinnedCursorKillTest(testParameters);
    }

    st.stop();
})();
