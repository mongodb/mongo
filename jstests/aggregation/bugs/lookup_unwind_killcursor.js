/**
 * Tests that the server can successfully kill the cursor of an aggregation pipeline which is
 * using a $lookup stage with its own cursor.
 *
 * This test was designed to reproduce SERVER-24386.
 */
(function() {
    'use strict';

    // Use a low batch size for the aggregation commands to ensure that the mongo shell does not
    // exhaust cursors during the first batch.
    const batchSize = 2;

    // Use a small batch in the $lookup stage to ensure it needs to issue a getMore. This will help
    // ensure that the $lookup stage has an open cursor when the first batch is returned.
    var conn =
        MongoRunner.runMongod({setParameter: "internalAggregationLookupBatchSize=" + batchSize});
    assert.neq(null, conn, 'mongod was unable to start up');
    const testDB = conn.getDB("test");

    function setup() {
        testDB.source.drop();
        testDB.dest.drop();

        assert.writeOK(testDB.source.insert({local: 1}));

        // The cursor batching logic actually requests one more document than it needs to fill the
        // first batch, so if we want to leave the $lookup stage paused with a cursor open we'll
        // need two more matching documents than the batch size.
        const numMatches = batchSize + 2;
        for (var i = 0; i < numMatches; ++i) {
            assert.writeOK(testDB.dest.insert({foreign: 1}));
        }
    }

    setup();

    const cmdObj = {
        aggregate: 'source',
        pipeline: [
            {
              $lookup: {
                  from: 'dest',
                  localField: 'local',
                  foreignField: 'foreign',
                  as: 'matches',
              }
            },
            {
              $unwind: {
                  path: '$matches',
              },
            },
        ],
        cursor: {
            batchSize: batchSize,
        },
    };

    var res = testDB.runCommand(cmdObj);
    assert.commandWorked(res);

    var cursor = new DBCommandCursor(conn, res, batchSize);
    cursor.close();  // Closing the cursor will issue a killCursor command.

    // Ensure the $lookup stage can be killed by dropping the collection.
    res = testDB.runCommand(cmdObj);
    assert.commandWorked(res);

    cursor = new DBCommandCursor(conn, res, batchSize);
    testDB.source.drop();

    assert.throws(function() {
        cursor.itcount();
    }, [], "expected cursor to have been destroyed during collection drop");

    // Ensure the $lookup stage can be killed by dropping the database.
    setup();
    res = testDB.runCommand(cmdObj);
    assert.commandWorked(res);

    cursor = new DBCommandCursor(conn, res, batchSize);
    assert.commandWorked(testDB.dropDatabase());

    assert.throws(function() {
        cursor.itcount();
    }, [], "expected cursor to have been destroyed during database drop");

    // Ensure the $lookup stage can be killed by the ClientCursorMonitor.
    setup();
    res = testDB.runCommand(cmdObj);
    assert.commandWorked(res);
    cursor = new DBCommandCursor(conn, res, batchSize);

    var serverStatus = assert.commandWorked(testDB.serverStatus());
    const expectedNumTimedOutCursors = serverStatus.metrics.cursor.timedOut + 1;

    // Wait until the idle cursor background job has killed the aggregation cursor.
    assert.commandWorked(testDB.adminCommand({setParameter: 1, cursorTimeoutMillis: 1000}));
    const cursorTimeoutFrequencySeconds = 4;
    assert.soon(
        function() {
            serverStatus = assert.commandWorked(testDB.serverStatus());
            // Use >= here since we may time out the $lookup stage's cursor as well.
            return serverStatus.metrics.cursor.timedOut >= expectedNumTimedOutCursors;
        },
        function() {
            return "aggregation cursor failed to time out: " + tojson(serverStatus);
        },
        cursorTimeoutFrequencySeconds * 1000 * 5);

    assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus));

    // We attempt to exhaust the aggregation cursor to verify that sending a getMore returns an
    // error due to the cursor being killed.
    var err = assert.throws(function() {
        cursor.itcount();
    });
    assert.eq(ErrorCodes.CursorNotFound, err.code, tojson(err));

    // Ensure the $lookup stage can be killed by shutting down the server.
    setup();
    res = testDB.runCommand(cmdObj);
    assert.commandWorked(res);

    assert.eq(0, MongoRunner.stopMongod(conn), "expected mongod to shutdown cleanly");
})();
