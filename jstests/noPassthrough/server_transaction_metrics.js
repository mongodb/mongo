// Tests multi-document transactions metrics in the serverStatus output.
// @tags: [uses_transactions]
(function() {
    "use strict";

    // Verifies that the server status response has the fields that we expect.
    function verifyServerStatusFields(serverStatusResponse) {
        assert(serverStatusResponse.hasOwnProperty("transactions"),
               "Expected the serverStatus response to have a 'transactions' field\n" +
                   tojson(serverStatusResponse));
        assert(serverStatusResponse.transactions.hasOwnProperty("currentActive"),
               "The 'transactions' field in serverStatus did not have the 'currentActive' field\n" +
                   tojson(serverStatusResponse.transactions));
        assert(
            serverStatusResponse.transactions.hasOwnProperty("currentInactive"),
            "The 'transactions' field in serverStatus did not have the 'currentInactive' field\n" +
                tojson(serverStatusResponse.transactions));
        assert(serverStatusResponse.transactions.hasOwnProperty("currentOpen"),
               "The 'transactions' field in serverStatus did not have the 'currentOpen' field\n" +
                   tojson(serverStatusResponse.transactions));
        assert(serverStatusResponse.transactions.hasOwnProperty("totalAborted"),
               "The 'transactions' field in serverStatus did not have the 'totalAborted' field\n" +
                   tojson(serverStatusResponse.transactions));
        assert(
            serverStatusResponse.transactions.hasOwnProperty("totalCommitted"),
            "The 'transactions' field in serverStatus did not have the 'totalCommitted' field\n" +
                tojson(serverStatusResponse.transactions));
        assert(serverStatusResponse.transactions.hasOwnProperty("totalStarted"),
               "The 'transactions' field in serverStatus did not have the 'totalStarted' field\n" +
                   tojson(serverStatusResponse.transactions));
    }

    // Verifies that the given value of the server status response is incremented in the way
    // we expect.
    function verifyServerStatusChange(initialStats, newStats, valueName, expectedIncrement) {
        assert.eq(initialStats[valueName] + expectedIncrement,
                  newStats[valueName],
                  "expected " + valueName + " to increase by " + expectedIncrement);
    }

    // Set up the replica set.
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // Set up the test database.
    const dbName = "test";
    const collName = "server_transactions_metrics";
    const testDB = primary.getDB(dbName);
    const adminDB = rst.getPrimary().getDB('admin');
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

    // Compare server status after starting a transaction with the server status before.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));

    let newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    // Verify that the open transaction counter is incremented while inside the transaction.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 1);
    // Verify that when not running an operation, the transaction is inactive.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 1);

    // Compare server status after the transaction commit with the server status before.
    assert.commandWorked(session.commitTransaction_forTesting());
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalStarted", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalCommitted", 1);
    // Verify that current open counter is decremented on commit.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 0);
    // Verify that both active and inactive are 0 on commit.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 0);

    // This transaction will abort.
    jsTest.log("Start a transaction and then abort it.");

    // Compare server status after starting a transaction with the server status before.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-2"}));

    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    // Verify that the open transaction counter is incremented while inside the transaction.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 1);
    // Verify that when not running an operation, the transaction is inactive.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 1);

    // Compare server status after the transaction abort with the server status before.
    assert.commandWorked(session.abortTransaction_forTesting());
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalStarted", 2);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalCommitted", 1);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalAborted", 1);
    // Verify that current open counter is decremented on abort.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 0);
    // Verify that both active and inactive are 0 on abort.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 0);

    // This transaction will abort due to a duplicate key insert.
    jsTest.log("Start a transaction that will abort on a duplicated key error.");

    // Compare server status after starting a transaction with the server status before.
    session.startTransaction();
    // Inserting a new document will work fine, and the transaction starts.
    assert.commandWorked(sessionColl.insert({_id: "insert-3"}));

    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    // Verify that the open transaction counter is incremented while inside the transaction.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 1);
    // Verify that when not running an operation, the transaction is inactive.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 1);

    // Compare server status after the transaction abort with the server status before.
    // The duplicated insert will fail, causing the transaction to abort.
    assert.commandFailedWithCode(sessionColl.insert({_id: "insert-3"}), ErrorCodes.DuplicateKey);
    // Ensure that the transaction was aborted on failure.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalStarted", 3);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalCommitted", 1);
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "totalAborted", 2);
    // Verify that current open counter is decremented on abort caused by an error.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 0);
    // Verify that both active and inactive are 0 on abort.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 0);

    // Hang the transaction on a failpoint in the middle of an operation to check active and
    // inactive counters while operation is running inside a transaction.
    jsTest.log(
        "Start a transaction that will hang in the middle of an operation due to a fail point.");
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangDuringBatchUpdate', mode: 'alwaysOn'}));

    const transactionFn = function() {
        const collName = 'server_transactions_metrics';
        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDb = session.getDatabase('test');
        const sessionColl = sessionDb[collName];

        session.startTransaction({readConcern: {level: 'snapshot'}});
        assert.commandWorked(sessionColl.update({}, {"update-1": 2}));
        assert.commandWorked(session.commitTransaction_forTesting());
    };
    const transactionProcess = startParallelShell(transactionFn, primary.port);

    // Keep running currentOp() until we see the transaction subdocument.
    assert.soon(function() {
        const transactionFilter =
            {active: true, 'lsid': {$exists: true}, 'transaction.parameters.txnNumber': {$eq: 0}};
        return 1 === adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).itcount();
    });
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    // Verify that the open transaction counter is incremented while inside the transaction.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 1);
    // Verify that the metrics show that the transaction is active while inside the operation.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 0);

    // Now the transaction can proceed.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangDuringBatchUpdate', mode: 'off'}));
    transactionProcess();
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    // Verify that current open counter is decremented on commit.
    verifyServerStatusChange(initialStatus.transactions, newStatus.transactions, "currentOpen", 0);
    // Verify that both active and inactive are 0 after the transaction finishes.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentActive", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentInactive", 0);

    // End the session and stop the replica set.
    session.endSession();
    rst.stopSet();
}());
