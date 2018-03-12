// Tests that destroying a snapshot read cursor aborts its transaction.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    // The cursor timeout thread runs every 'cursorMonitorFrequencySecs' seconds, timing out cursors
    // that have been inactive for 'cursorTimeoutMs' milliseconds.
    const cursorTimeoutMs = 2000;
    const cursorMonitorFrequencySecs = 1;

    const options = {
        setParameter: {
            // We use the "cursorTimeoutMillis" server parameter to decrease how long it takes for a
            // non-exhausted cursor to time out. We use the "clientCursorMonitorFrequencySecs"
            // server parameter to make the ClientCursorMonitor that cleans up the timed out cursors
            // run more often. The combination of these server parameters reduces the amount of time
            // we need to wait within this test.
            cursorTimeoutMillis: cursorTimeoutMs,
            clientCursorMonitorFrequencySecs: cursorMonitorFrequencySecs,
        }
    };

    const rst = new ReplSetTest({nodes: 1, nodeOptions: options});
    rst.startSet();
    rst.initiate();

    const primaryDB = rst.getPrimary().getDB(dbName);
    if (!primaryDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    const session = primaryDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    let txnNumber = 0;

    //
    // Test killCursors.
    //

    for (let i = 0; i < 4; i++) {
        sessionDB.coll.insert({_id: i}, {writeConcern: {w: "majority"}});
    }

    // Create a snapshot read cursor.
    let res = assert.commandWorked(sessionDB.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber)
    }));

    // It should not be possible to drop the collection.
    assert.commandFailedWithCode(sessionDB.runCommand({drop: collName, maxTimeMS: 500}),
                                 ErrorCodes.ExceededTimeLimit);

    // Kill the cursor.
    assert.commandWorked(sessionDB.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    // It should be possible to drop the collection.
    sessionDB.coll.drop();

    //
    // Test legacy killCursors.
    //

    for (let i = 0; i < 4; i++) {
        sessionDB.coll.insert({_id: i}, {writeConcern: {w: "majority"}});
    }

    // Create a snapshot read cursor.
    res = assert.commandWorked(sessionDB.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber)
    }));

    // It should not be possible to drop the collection.
    assert.commandFailedWithCode(sessionDB.runCommand({drop: collName, maxTimeMS: 500}),
                                 ErrorCodes.ExceededTimeLimit);

    // Kill the cursor using legacy.
    primaryDB.getMongo().forceReadMode("legacy");
    let cursor = new DBCommandCursor(primaryDB, res);
    cursor.close();
    primaryDB.getMongo().forceReadMode("commands");

    // It should be possible to drop the collection.
    sessionDB.coll.drop();

    //
    // Test cursor timeout.
    //

    for (let i = 0; i < 4; i++) {
        sessionDB.coll.insert({_id: i}, {writeConcern: {w: "majority"}});
    }

    let serverStatus = assert.commandWorked(sessionDB.serverStatus());
    const expectedNumTimedOutCursors = serverStatus.metrics.cursor.timedOut + 1;

    // Create a snapshot read cursor.
    res = assert.commandWorked(sessionDB.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber)
    }));

    // It should not be possible to drop the collection.
    assert.commandFailedWithCode(sessionDB.runCommand({drop: collName, maxTimeMS: 500}),
                                 ErrorCodes.ExceededTimeLimit);

    // Wait until the idle cursor background job has killed the cursor.
    assert.soon(
        function() {
            serverStatus = assert.commandWorked(sessionDB.serverStatus());
            return +serverStatus.metrics.cursor.timedOut === expectedNumTimedOutCursors;
        },
        function() {
            return "cursor failed to time out: " + tojson(serverStatus.metrics.cursor);
        });
    assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus));

    // Verify that the cursor was killed.
    assert.commandFailedWithCode(
        sessionDB.runCommand(
            {getMore: res.cursor.id, collection: collName, txnNumber: NumberLong(txnNumber)}),
        ErrorCodes.CursorNotFound);

    // It should be possible to drop the collection.
    sessionDB.coll.drop();

    session.endSession();
    rst.stopSet();
})();
