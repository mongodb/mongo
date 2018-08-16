// Tests multi-document transactions metrics in the serverStatus output.
// @tags: [uses_transactions]
(function() {
    "use strict";

    // Verifies that the server status response has the fields that we expect.
    function verifyServerStatusFields(serverStatusResponse) {
        assert(serverStatusResponse.hasOwnProperty("transactions"),
               "Expected the serverStatus response to have a 'transactions' field\n" +
                   serverStatusResponse);
        assert(serverStatusResponse.transactions.hasOwnProperty("totalAborted"),
               "The 'transactions' field in serverStatus did not have the 'totalAborted' field\n" +
                   serverStatusResponse.transactions);
        assert(
            serverStatusResponse.transactions.hasOwnProperty("totalCommitted"),
            "The 'transactions' field in serverStatus did not have the 'totalCommitted' field\n" +
                serverStatusResponse.transactions);
        assert(serverStatusResponse.transactions.hasOwnProperty("totalStarted"),
               "The 'transactions' field in serverStatus did not have the 'totalStarted' field\n" +
                   serverStatusResponse.transactions);
    }

    // Verifies that the given value of the server status response is incremented in the way
    // we expect.
    function verifyServerStatusChange(initialStats, newStats, valueName, expectedIncrement) {
        assert.eq(initialStats[valueName] + expectedIncrement,
                  newStats[valueName],
                  "expected " + valueName + " to increase by " + expectedIncrement);
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // Set up the test database.
    const dbName = "test";
    const collName = "server_transactions_metrics";
    const testDB = primary.getDB(dbName);
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Start the session.
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    // Get state of server status before the transaction.
    let initialStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(initialStatus);

    // This transaction will commit.
    jsTest.log("Start a transaction and then commit it.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));
    session.commitTransaction();

    // Compare server status after the transaction commit with the server status before.
    let newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalStarted", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalCommitted", 1);

    // This transaction will abort.
    jsTest.log("Start a transaction and then abort it.");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-2"}));
    session.abortTransaction();

    // Compare server status after the transaction abort with the server status before.
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalStarted", 2);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalCommitted", 1);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalAborted", 1);

    // This transaction will abort due to a duplicate key insert.
    jsTest.log("Start a transaction that will abort on a duplicated key error.");
    session.startTransaction();
    // Inserting a new document will work fine, and the transaction starts.
    assert.commandWorked(sessionColl.insert({_id: "insert-3"}));
    // The duplicated insert will fail, causing the transaction to abort.
    assert.commandFailedWithCode(sessionColl.insert({_id: "insert-3"}), ErrorCodes.DuplicateKey);
    // Ensure that the transaction was aborted on failure.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Compare server status after the transaction abort with the server status before.
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalStarted", 3);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalCommitted", 1);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalAborted", 2);

    session.endSession();
    rst.stopSet();
}());
