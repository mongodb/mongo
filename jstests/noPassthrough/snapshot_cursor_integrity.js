// Tests that a cursor is iterated in a transaction/session iff it was created in that
// transaction/session. Specifically tests this in the context of snapshot cursors.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primaryDB = rst.getPrimary().getDB(dbName);
    if (!primaryDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    const session1 = primaryDB.getMongo().startSession();
    const sessionDB1 = session1.getDatabase(dbName);

    const session2 = primaryDB.getMongo().startSession();
    const sessionDB2 = session2.getDatabase(dbName);

    const bulk = primaryDB.coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 10; ++i) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    // Establish a snapshot cursor in session1.
    let res = assert.commandWorked(sessionDB1.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(0),
        autocommit: false,
        startTransaction: true,
        batchSize: 2
    }));
    assert(res.hasOwnProperty("cursor"));
    assert(res.cursor.hasOwnProperty("id"));
    let cursorID = res.cursor.id;

    // The cursor may not be iterated outside of any session.
    assert.commandFailedWithCode(
        primaryDB.runCommand({getMore: cursorID, collection: collName, batchSize: 2}), 50737);

    // The cursor can still be iterated in session1.
    assert.commandWorked(sessionDB1.runCommand({
        getMore: cursorID,
        collection: collName,
        autocommit: false,
        txnNumber: NumberLong(0),
        batchSize: 2
    }));

    // The cursor may not be iterated in a different session.
    assert.commandFailedWithCode(
        sessionDB2.runCommand({getMore: cursorID, collection: collName, batchSize: 2}), 50738);

    // The cursor can still be iterated in session1.
    assert.commandWorked(sessionDB1.runCommand({
        getMore: cursorID,
        collection: collName,
        autocommit: false,
        txnNumber: NumberLong(0),
        batchSize: 2
    }));

    // The cursor may not be iterated outside of any transaction.
    assert.commandFailedWithCode(
        sessionDB1.runCommand({getMore: cursorID, collection: collName, batchSize: 2}), 50740);

    // The cursor can still be iterated in its transaction in session1.
    assert.commandWorked(sessionDB1.runCommand({
        getMore: cursorID,
        collection: collName,
        autocommit: false,
        txnNumber: NumberLong(0),
        batchSize: 2
    }));

    // The cursor may not be iterated in a different transaction on session1.
    assert.commandWorked(sessionDB1.runCommand({
        find: collName,
        txnNumber: NumberLong(1),
        autocommit: false,
        readConcern: {level: "snapshot"},
        startTransaction: true
    }));
    assert.commandFailedWithCode(sessionDB1.runCommand({
        getMore: cursorID,
        collection: collName,
        autocommit: false,
        txnNumber: NumberLong(1),
        batchSize: 2
    }),
                                 50741);

    // The cursor can no longer be iterated because its transaction has ended.
    assert.commandFailedWithCode(sessionDB1.runCommand({
        getMore: cursorID,
        collection: collName,
        autocommit: false,
        txnNumber: NumberLong(0),
        batchSize: 2
    }),
                                 ErrorCodes.TransactionTooOld);

    // Kill the cursor.
    assert.commandWorked(
        sessionDB1.runCommand({killCursors: sessionDB1.coll.getName(), cursors: [cursorID]}));

    // Establish a cursor outside of any transaction in session1.
    res = assert.commandWorked(sessionDB1.runCommand({find: collName, batchSize: 2}));
    assert(res.hasOwnProperty("cursor"));
    assert(res.cursor.hasOwnProperty("id"));
    cursorID = res.cursor.id;

    // The cursor may not be iterated inside a transaction.
    assert.commandWorked(sessionDB1.runCommand({
        find: collName,
        txnNumber: NumberLong(2),
        autocommit: false,
        readConcern: {level: "snapshot"},
        startTransaction: true
    }));
    assert.commandFailedWithCode(sessionDB1.runCommand({
        getMore: cursorID,
        collection: collName,
        autocommit: false,
        txnNumber: NumberLong(2),
        batchSize: 2
    }),
                                 50739);

    // The cursor can still be iterated outside of any transaction. Exhaust the cursor.
    assert.commandWorked(sessionDB1.runCommand({getMore: cursorID, collection: collName}));

    // Establish a cursor outside of any session.
    res = assert.commandWorked(primaryDB.runCommand({find: collName, batchSize: 2}));
    assert(res.hasOwnProperty("cursor"));
    assert(res.cursor.hasOwnProperty("id"));
    cursorID = res.cursor.id;

    // The cursor may not be iterated inside a session.
    assert.commandFailedWithCode(
        sessionDB1.runCommand({getMore: cursorID, collection: collName, batchSize: 2}), 50736);

    // The cursor can still be iterated outside of any session. Exhaust the cursor.
    assert.commandWorked(primaryDB.runCommand({getMore: cursorID, collection: collName}));

    session1.endSession();
    session2.endSession();
    rst.stopSet();
})();
