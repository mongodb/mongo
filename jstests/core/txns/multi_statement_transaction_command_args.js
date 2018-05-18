/**
 * Verify that multi-statement transaction command arguments behave correctly.
 *
 * @tags: [uses_transactions]
 */

(function() {
    "use strict";
    load('jstests/libs/uuid_util.js');

    // Makes assertions on commands run without logical session ids.
    TestData.disableImplicitSessions = true;

    const dbName = "test";
    const collName = "multi_statement_transaction_command_args";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];
    let txnNumber = 0;

    // Set up the test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

    // Initiate the session.
    const sessionOptions = {causalConsistency: false};
    let session = db.getMongo().startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);

    /***********************************************************************************************
     * Verify that fields are not accepted unless their preconditional fields are present in
     * this hierarchy: lsid -> txnNumber -> autocommit -> startTransaction
     * Omitted fields are commented out explicitly.
     **********************************************************************************************/

    // lsid -> txnNumber.
    // Running a command through 'sessionDb' implicitly attaches an 'lsid' to commands,
    // so 'testDB' is used instead.
    jsTestLog("Try to begin a transaction with txnNumber but no lsid");
    txnNumber++;
    let res = assert.commandFailedWithCode(testDB.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        // autocommit: false,
        // startTransaction: true
    }),
                                           ErrorCodes.InvalidOptions);
    assert(res.errmsg.includes("Transaction number requires a session ID"));

    // txnNumber -> autocommit
    jsTestLog("Try to begin a transaction with autocommit but no txnNumber");
    txnNumber++;
    res = assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        // txnNumber: NumberLong(txnNumber),
        autocommit: false,
        // startTransaction: true
    }),
                                       ErrorCodes.InvalidOptions);
    assert(res.errmsg.includes("'autocommit' field requires a transaction number"));

    // autocommit -> startTransaction
    jsTestLog("Try to begin a transaction with startTransaction but no autocommit");
    txnNumber++;
    res = assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        // autocommit: false,
        startTransaction: true
    }),
                                       ErrorCodes.InvalidOptions);
    assert(res.errmsg.includes("'startTransaction' field requires 'autocommit' field"));

    /***********************************************************************************************
     * Verify that the 'startTransaction' argument works correctly.
     **********************************************************************************************/

    jsTestLog("Begin a transaction with startTransaction=true and autocommit=false");
    txnNumber++;

    // Start the transaction.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // Commit the transaction.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"}
    }));

    jsTestLog("Try to start an already in progress transaction.");
    txnNumber++;

    // Start the transaction.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // Try to start the transaction again.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.ConflictingOperationInProgress);

    // Commit the transaction.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"}
    }));

    jsTestLog(
        "Try to begin a transaction by omitting 'startTransaction' and setting autocommit=false");
    txnNumber++;
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Try to begin a transaction with startTransaction=false and autocommit=false");
    txnNumber++;
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: false,
        autocommit: false
    }),
                                 ErrorCodes.InvalidOptions);

    /***********************************************************************************************
     * Setting autocommit=true or omitting autocommit on a transaction operation fails.
     **********************************************************************************************/

    jsTestLog("Run an initial transaction operation with autocommit=true");
    txnNumber++;

    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: true
    }),
                                 ErrorCodes.InvalidOptions);

    // Committing the transaction should fail.
    assert.commandFailedWithCode(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Run a non-initial transaction operation with autocommit=true");
    txnNumber++;

    // Start the transaction with an insert.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // Try to execute a transaction operation with autocommit=true. It should fail without affecting
    // the transaction.
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: txnNumber + "_1"}],
        txnNumber: NumberLong(txnNumber),
        autocommit: true
    }),
                                 ErrorCodes.InvalidOptions);

    // Try to execute a transaction operation without an autocommit field. It should fail without
    // affecting the transaction.
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: txnNumber + "_2"}],
        txnNumber: NumberLong(txnNumber),
    }),
                                 ErrorCodes.InvalidOptions);

    // Committing the transaction should succeed.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"}
    }));

    /***********************************************************************************************
     * Invalid to include autocommit field on an operation not inside a transaction.
     **********************************************************************************************/

    jsTestLog("Run an operation with autocommit=false outside of a transaction.");
    txnNumber++;

    assert.commandWorked(
        sessionDb.runCommand({find: collName, filter: {}, txnNumber: NumberLong(txnNumber)}));

    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}),
        ErrorCodes.InvalidOptions);

    /***********************************************************************************************
     * The 'autocommit' field must be specified on commit/abort commands.
     **********************************************************************************************/

    jsTestLog("Run a commitTransaction command with valid and invalid 'autocommit' field values.");
    txnNumber++;

    // Start the transaction with an insert.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // Committing the transaction should fail if 'autocommit' is omitted.
    assert.commandFailedWithCode(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.InvalidOptions);

    // Committing the transaction should fail if autocommit=true.
    assert.commandFailedWithCode(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        writeConcern: {w: "majority"},
        autocommit: true
    }),
                                 ErrorCodes.InvalidOptions);

    // Committing the transaction should succeed.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"}
    }));

    jsTestLog("Run an abortTransaction command with and without an 'autocommit' field");
    txnNumber++;

    // Start the transaction with an insert.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }));

    // Aborting the transaction should fail if 'autocommit' is omitted.
    assert.commandFailedWithCode(
        sessionDb.adminCommand({abortTransaction: 1, txnNumber: NumberLong(txnNumber)}),
        ErrorCodes.InvalidOptions);

    // Aborting the transaction should fail if autocommit=true.
    assert.commandFailedWithCode(
        sessionDb.adminCommand(
            {abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: true}),
        ErrorCodes.InvalidOptions);

    // Aborting the transaction should succeed.
    assert.commandWorked(sessionDb.adminCommand(
        {abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));

}());
