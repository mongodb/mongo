// Tests that stashed transaction resources are destroyed at shutdown and stepdown.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    //
    // Test that stashed transaction resources are destroyed at shutdown.
    //

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let primaryDB = rst.getPrimary().getDB(dbName);
    if (!primaryDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    let session = primaryDB.getMongo().startSession();
    let sessionDB = session.getDatabase(dbName);

    for (let i = 0; i < 4; i++) {
        assert.commandWorked(sessionDB.coll.insert({_id: i}, {writeConcern: {w: "majority"}}));
    }

    // Create a snapshot read cursor.
    assert.commandWorked(sessionDB.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        startTransaction: true,
        autocommit: false,
        txnNumber: NumberLong(0)
    }));

    // It should be possible to shut down the server without hanging. We must skip collection
    // validation, since this will hang.
    const signal = true;  // Use default kill signal.
    const forRestart = false;
    rst.stopSet(signal, forRestart, {skipValidation: true});

    function testStepdown(stepdownFunc) {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();

        const primary = rst.getPrimary();
        const primaryDB = primary.getDB(dbName);

        const session = primaryDB.getMongo().startSession();
        const sessionDB = session.getDatabase(dbName);

        for (let i = 0; i < 4; i++) {
            assert.commandWorked(sessionDB.coll.insert({_id: i}, {writeConcern: {w: "majority"}}));
        }

        // Create a snapshot read cursor.
        const res = assert.commandWorked(sessionDB.runCommand({
            find: collName,
            batchSize: 2,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(0),
            startTransaction: true,
            autocommit: false
        }));
        assert(res.hasOwnProperty("cursor"), tojson(res));
        assert(res.cursor.hasOwnProperty("id"), tojson(res));
        const cursorId = res.cursor.id;

        // It should be possible to step down the primary without hanging.
        stepdownFunc(rst);
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);

        // Perform a getMore using the previous transaction's open cursorId. We expect to receive
        // CursorNotFound if the cursor was properly closed on step down.
        assert.commandFailedWithCode(
            sessionDB.runCommand({getMore: cursorId, collection: collName}),
            ErrorCodes.CursorNotFound);
        rst.stopSet();
    }

    //
    // Test that stashed transaction resources are destroyed at stepdown triggered by
    // replSetStepDown.
    //
    function replSetStepDown(replSetTest) {
        assert.throws(function() {
            replSetTest.getPrimary().adminCommand({replSetStepDown: 60, force: true});
        });
    }
    testStepdown(replSetStepDown);

    //
    // Test that stashed transaction resources are destroyed at stepdown triggered by loss of
    // quorum.
    //
    function stepDownOnLossOfQuorum(replSetTest) {
        const secondary = rst.getSecondary();
        const secondaryId = rst.getNodeId(secondary);
        rst.stop(secondaryId);
    }
    testStepdown(stepDownOnLossOfQuorum);
})();
