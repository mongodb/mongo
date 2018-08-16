// Tests transactions metrics in the serverStatus output.
// @tags: [uses_transactions]
(function() {
    "use strict";
    function verifyServerStatusFields(serverStatusResponse) {
        assert(serverStatusResponse.hasOwnProperty("transactions"),
               "Expected the serverStatus response to have a 'transactions' field\n" +
                   serverStatusResponse);
        assert(serverStatusResponse.transactions.hasOwnProperty("totalStarted"),
               "The 'transactions' field in serverStatus did not have the totalStarted field\n" +
                   serverStatusResponse.transactions);
    }

    function verifyServerStatusChanges(initialStats, newStats, valueName, newValue) {
        assert.eq(initialStats[valueName] + newValue,
                  newStats[valueName],
                  "expected " + valueName + " to increase by " + newValue);
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // Set up the test database.
    const dbName = "test";
    const collName = "server_transactions_metrics";
    const testDB = primary.getDB(dbName);
    testDB.dropDatabase();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Get state of server status before the transaction.
    let initialStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(initialStatus);

    // Start the transaction.
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));
    session.commitTransaction();

    // Compare server status after the transaction with the server status before.
    let newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChanges(
        initialStatus.transactions, newStatus.transactions, "totalStarted", 1);

    session.endSession();
    rst.stopSet();
}());
