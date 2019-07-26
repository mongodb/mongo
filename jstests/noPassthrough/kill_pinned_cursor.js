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

load("jstests/libs/fixture_helpers.js");     // For "isMongos".
load("jstests/libs/pin_getmore_cursor.js");  // For "withPinnedCursor".
const st = new ShardingTest({shards: 2});

// Enables the specified 'failPointName', executes 'runGetMoreFunc' function in a parallel
// shell, waits for the the failpoint to be hit, then kills the cursor and confirms that the
// kill was successful.
function runPinnedCursorKillTest({conn, failPointName, runGetMoreFunc}) {
    function assertFunction(cursorId, coll) {
        const db = coll.getDB();
        // Kill the cursor associated with the command and assert that the kill succeeded.
        let cmdRes = db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
        assert.commandWorked(cmdRes);
        assert.eq(cmdRes.cursorsKilled, [cursorId]);
        assert.eq(cmdRes.cursorsAlive, []);
        assert.eq(cmdRes.cursorsNotFound, []);
        assert.eq(cmdRes.cursorsUnknown, []);
    }
    withPinnedCursor({
        conn: conn,
        sessionId: null,
        db: conn.getDB("test"),
        assertFunction: assertFunction,
        runGetMoreFunc: runGetMoreFunc,
        failPointName: failPointName,
        assertEndCounts: true
    });
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
    cmdObj: {configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "alwaysOn"}
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
