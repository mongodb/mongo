/**
 * Verify that readConcern and writeConcern are not allowed in transactions other than the
 * first statement (for readConcern) and the commit (for writeConcern)
 *
 * @tags: [uses_transactions, uses_snapshot_read_concern]
 */

(function() {
    "use strict";
    const dbName = "test";
    const collName = "no_read_or_write_concerns_inside_txn";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    // Set up the test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    // Initiate the session.
    const sessionOptions = {causalConsistency: false};
    let session = db.getMongo().startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);
    let txnNumber = 0;
    let stmtId = 0;

    jsTestLog("Starting first transaction");
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 0}],
        readConcern: {level: "snapshot"},
        startTransaction: true,
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }));

    jsTestLog("Attempting to insert with readConcern: snapshot within a transaction.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 1}],
        readConcern: {level: "snapshot"},
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Attempting to insert with readConcern:majority within a transaction.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 2}],
        readConcern: {level: "majority"},
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Attempting to insert with readConcern:local within a transaction.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 3}],
        readConcern: {level: "local"},
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Transaction should still commit.");
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        autocommit: false,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }));
    assert.sameMembers(testColl.find().toArray(), [{_id: 0}]);

    // Drop and re-create collection to keep parts of test isolated from one another.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    txnNumber++;
    stmtId = 0;

    jsTestLog("Attempting to start transaction with local writeConcern.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 4}],
        readConcern: {level: "snapshot"},
        writeConcern: {w: 1},
        startTransaction: true,
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);
    txnNumber++;
    stmtId = 0;

    jsTestLog("Attempting to start transaction with majority writeConcern.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 5}],
        readConcern: {level: "snapshot"},
        writeConcern: {w: "majority"},
        startTransaction: true,
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);
    txnNumber++;
    stmtId = 0;

    jsTestLog("Starting transaction normally.");
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 6}],
        readConcern: {level: "snapshot"},
        startTransaction: true,
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }));

    jsTestLog("Attempting to write within transaction with majority write concern.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 7}],
        writeConcern: {w: "majority"},
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Attempting to write within transaction with local write concern.");
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 8}],
        writeConcern: {w: 1},
        autocommit: false,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Transaction should still commit.");
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        autocommit: false,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++)
    }));
    assert.sameMembers(testColl.find().toArray(), [{_id: 6}]);
    session.endSession();
}());
