/**
 * Tests that an aggregation cursor is killed when it is timed out by the ClientCursorMonitor.
 *
 * This test was designed to reproduce SERVER-25585.
 */
(function() {
    'use strict';

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
        }
    };
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

    const testDB = conn.getDB('test');

    // We use a batch size of 2 to ensure that the mongo shell does not exhaust the cursor on its
    // first batch.
    const batchSize = 2;
    const numMatches = 5;

    function assertCursorTimesOut(collName, pipeline) {
        const res = assert.commandWorked(testDB.runCommand({
            aggregate: collName,
            pipeline: pipeline,
            cursor: {
                batchSize: batchSize,
            },
        }));

        let serverStatus = assert.commandWorked(testDB.serverStatus());
        const expectedNumTimedOutCursors = serverStatus.metrics.cursor.timedOut + 1;

        const cursor = new DBCommandCursor(conn, res, batchSize);

        // Wait until the idle cursor background job has killed the aggregation cursor.
        assert.soon(
            function() {
                serverStatus = assert.commandWorked(testDB.serverStatus());
                return +serverStatus.metrics.cursor.timedOut === expectedNumTimedOutCursors;
            },
            function() {
                return "aggregation cursor failed to time out: " + tojson(serverStatus);
            });

        assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus));

        // We attempt to exhaust the aggregation cursor to verify that sending a getMore returns an
        // error due to the cursor being killed.
        let err = assert.throws(function() {
            cursor.itcount();
        });
        assert.eq(ErrorCodes.CursorNotFound, err.code, tojson(err));
    }

    assert.writeOK(testDB.source.insert({local: 1}));
    for (let i = 0; i < numMatches; ++i) {
        assert.writeOK(testDB.dest.insert({foreign: 1}));
    }

    // Test that a regular aggregation cursor is killed when the timeout is reached.
    assertCursorTimesOut('dest', []);

    // Test that an aggregation cursor with a $lookup stage is killed when the timeout is reached.
    assertCursorTimesOut('source', [
        {
          $lookup: {
              from: 'dest',
              localField: 'local',
              foreignField: 'foreign',
              as: 'matches',
          }
        },
        {
          $unwind: "$matches",
        },
    ]);

    MongoRunner.stopMongod(conn);
})();
