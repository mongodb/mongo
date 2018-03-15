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
        txnNumber: NumberLong(0)
    }));

    // It should be possible to shut down the server without hanging. We must skip collection
    // validation, since this will hang.
    const signal = true;  // Use default kill signal.
    const forRestart = false;
    rst.stopSet(signal, forRestart, {skipValidation: true});

    //
    // Test that stashed transaction resources are destroyed at stepdown.
    //

    rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    primaryDB = primary.getDB(dbName);

    session = primaryDB.getMongo().startSession();
    sessionDB = session.getDatabase(dbName);

    for (let i = 0; i < 4; i++) {
        assert.commandWorked(sessionDB.coll.insert({_id: i}, {writeConcern: {w: "majority"}}));
    }

    // Create a snapshot read cursor.
    const res = assert.commandWorked(sessionDB.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(0)
    }));
    assert(res.hasOwnProperty("cursor"), tojson(res));
    assert(res.cursor.hasOwnProperty("id"), tojson(res));

    // It should be possible to step down the primary without hanging.
    assert.throws(function() {
        primary.adminCommand({replSetStepDown: 60, force: true});
    });
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);

    // TODO SERVER-33690: Destroying stashed transaction resources should kill the cursor, so this
    // getMore should fail.
    assert.commandWorked(sessionDB.runCommand(
        {getMore: res.cursor.id, collection: collName, txnNumber: NumberLong(0)}));

    rst.stopSet();
})();
