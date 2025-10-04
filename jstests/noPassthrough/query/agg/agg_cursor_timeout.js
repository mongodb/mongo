/**
 * Tests that an aggregation cursor is killed when it is timed out by the ClientCursorMonitor.
 *
 * This test was designed to reproduce SERVER-25585 and BF-37637.
 */
// Cursor timeout on mongod is handled by a single thread/timer that will sleep for
// "clientCursorMonitorFrequencySecs" and add the sleep value to each operation's duration when
// it wakes up, timing out those whose "now() - last accessed since" time exceeds. A cursor
// timeout of 2 seconds with a monitor frequency of 1 second means an effective timeout period
// of 1 to 2 seconds.
const cursorTimeoutMs = 2000;
const cursorMonitorFrequencySecs = 1;

const options = {
    setParameter: {
        internalDocumentSourceCursorBatchSizeBytes: 1,
        // We use the "cursorTimeoutMillis" server parameter to decrease how long it takes for a
        // non-exhausted cursor to time out. We use the "clientCursorMonitorFrequencySecs"
        // server parameter to make the ClientCursorMonitor that cleans up the timed out cursors
        // run more often. The combination of these server parameters reduces the amount of time
        // we need to wait within this test.
        cursorTimeoutMillis: cursorTimeoutMs,
        clientCursorMonitorFrequencySecs: cursorMonitorFrequencySecs,
    },
};
const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

const testDB = conn.getDB("test");

// We use a batch size of 2 to ensure that the mongo shell does not exhaust the cursor on its
// first batch.
const batchSize = 2;
const numMatches = 5;

function assertCursorTimesOutImpl(collName, pipeline) {
    const res = assert.commandWorked(
        testDB.runCommand({
            aggregate: collName,
            pipeline: pipeline,
            cursor: {
                batchSize: batchSize,
            },
        }),
    );

    let serverStatus = assert.commandWorked(testDB.serverStatus());
    const expectedNumTimedOutCursors = serverStatus.metrics.cursor.timedOut + 1;

    const cursor = new DBCommandCursor(testDB, res, batchSize);

    // Wait until the idle cursor background job has killed the aggregation cursor.
    assert.soon(
        function () {
            serverStatus = assert.commandWorked(testDB.serverStatus());
            return +serverStatus.metrics.cursor.timedOut === expectedNumTimedOutCursors;
        },
        function () {
            return "aggregation cursor failed to time out: " + tojson(serverStatus.metrics.cursor);
        },
    );

    assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus));

    // We attempt to exhaust the aggregation cursor to verify that sending a getMore returns an
    // error due to the cursor being killed.
    let err = assert.throws(function () {
        cursor.itcount();
    });
    assert.eq(ErrorCodes.CursorNotFound, err.code, tojson(err));
}

function assertCursorTimesOut(collName, pipeline) {
    // Confirm that cursor timeout occurs outside of sessions.
    TestData.disableImplicitSessions = true;
    assertCursorTimesOutImpl(collName, pipeline);
    TestData.disableImplicitSessions = false;

    // Confirm that cursor timeout occurs within sessions when the
    // `enableTimeoutOfInactiveSessionCursors` parameter is set to true. If false, we rely on
    // session expiration to cleanup outstanding cursors.
    assert.commandWorked(testDB.adminCommand({setParameter: 1, enableTimeoutOfInactiveSessionCursors: true}));
    assertCursorTimesOutImpl(collName, pipeline);
    assert.commandWorked(testDB.adminCommand({setParameter: 1, enableTimeoutOfInactiveSessionCursors: false}));
}

assert.commandWorked(testDB.source.insert({local: 1}));
for (let i = 0; i < numMatches; ++i) {
    assert.commandWorked(testDB.dest.insert({foreign: 1}));
}

// Test that a regular aggregation cursor is killed when the timeout is reached.
assertCursorTimesOut("dest", []);

// Test that an aggregation cursor with a $lookup stage is killed when the timeout is reached.
assertCursorTimesOut("source", [
    {
        $lookup: {
            from: "dest",
            localField: "local",
            foreignField: "foreign",
            as: "matches",
        },
    },
    {
        $unwind: "$matches",
    },
]);

// Test that an aggregation cursor with nested $lookup stages is killed when the timeout is
// reached.
assertCursorTimesOut("source", [
    {
        $lookup: {
            from: "dest",
            let: {local1: "$local"},
            pipeline: [
                {$match: {$expr: {$eq: ["$foreign", "$$local1"]}}},
                {
                    $lookup: {
                        from: "source",
                        let: {foreign1: "$foreign"},
                        pipeline: [{$match: {$expr: {$eq: ["$local", "$$foreign1"]}}}],
                        as: "matches2",
                    },
                },
                {
                    $unwind: "$matches2",
                },
            ],
            as: "matches1",
        },
    },
    {
        $unwind: "$matches1",
    },
]);

// Test that an aggregation cursor with a $group inside $lookup subpipeline is killed when timeout
// is reached. Designed to reproduce BF-37637.
testDB.dest.drop();
for (let i = 0; i < numMatches; ++i) {
    assert.commandWorked(testDB.dest.insert({foreign: 1, category: i % 2, value: i}));
}

assertCursorTimesOut("source", [
    {
        $lookup: {
            from: "dest",
            let: {localVal: "$local"},
            pipeline: [
                {$match: {$expr: {$eq: ["$foreign", "$$localVal"]}}},
                {
                    $group: {
                        _id: "$category",
                        count: {$sum: 1},
                        values: {$push: "$value"},
                    },
                },
            ],
            as: "groupedMatches",
        },
    },
    {$unwind: "$groupedMatches"},
]);

MongoRunner.stopMongod(conn);
