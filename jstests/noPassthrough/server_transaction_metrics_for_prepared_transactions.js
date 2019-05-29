/**
 * Tests prepared transactions metrics in the serverStatus output.
 * @tags: [uses_transactions, uses_prepare_transaction, exclude_from_large_txns]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/rslib.js");

    /**
     * Verifies that the serverStatus response has the fields that we expect.
     */
    function verifyServerStatusFields(serverStatusResponse) {
        assert(serverStatusResponse.hasOwnProperty("transactions"),
               "Expected the serverStatus response to have a 'transactions' field: " +
                   tojson(serverStatusResponse));
        assert(serverStatusResponse.transactions.hasOwnProperty("totalPrepared"),
               "Expected the serverStatus response to have a 'totalPrepared' field: " +
                   tojson(serverStatusResponse));
        assert(serverStatusResponse.transactions.hasOwnProperty("totalPreparedThenCommitted"),
               "Expected the serverStatus response to have a 'totalPreparedThenCommitted' field: " +
                   tojson(serverStatusResponse));
        assert(serverStatusResponse.transactions.hasOwnProperty("totalPreparedThenAborted"),
               "Expected the serverStatus response to have a 'totalPreparedThenAborted' field: " +
                   tojson(serverStatusResponse));
        assert(serverStatusResponse.transactions.hasOwnProperty("currentPrepared"),
               "Expected the serverStatus response to have a 'currentPrepared' field: " +
                   tojson(serverStatusResponse));
    }

    /**
     * Verifies that the given value of the server status response is incremented in the way
     * we expect.
     */
    function verifyServerStatusChange(initialStats, newStats, valueName, expectedIncrement) {
        assert.eq(initialStats[valueName] + expectedIncrement,
                  newStats[valueName],
                  "expected " + valueName + " to increase by " + expectedIncrement);
    }

    /**
     * Verifies that the timestamp of the oldest active transaction in the transactions table
     * is greater than the lower bound and less than or equal to the upper bound.
     */
    function verifyOldestActiveTransactionTimestamp(testDB, lowerBound, upperBound) {
        let res = assert.commandWorked(
            testDB.getSiblingDB("config").getCollection("transactions").runCommand("find", {
                "filter": {"state": {"$in": ["prepared", "inProgress"]}},
                "sort": {"startOpTime": 1},
                "readConcern": {"level": "local"},
                "limit": 1
            }));

        let entry = res.cursor.firstBatch[0];
        assert.neq(undefined, entry);

        assert.lt(lowerBound,
                  entry.startOpTime.ts,
                  "oldest active transaction timestamp should be greater than the lower bound");
        assert.lte(
            entry.startOpTime.ts,
            upperBound,
            "oldest active transaction timestamp should be less than or equal to the upper bound");
    }

    // Set up the replica set.
    const rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // Set up the test database.
    const dbName = "test";
    const collName = "server_transactions_metrics_for_prepared_transactions";
    const testDB = primary.getDB(dbName);
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Start the session.
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    // Get state of server status before the transaction.
    const initialStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(initialStatus);

    // Test server metrics for a prepared transaction that is committed.
    jsTest.log("Prepare a transaction and then commit it");

    const doc1 = {_id: 1, x: 1};

    // Start transaction and prepare transaction.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc1));

    const opTimeBeforePrepareForCommit = getLastOpTime(primary);
    const prepareTimestampForCommit = PrepareHelpers.prepareTransaction(session);

    // Verify the total and current prepared transaction counter is updated and the oldest active
    // oplog entry timestamp is shown.
    let newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPrepared", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentPrepared", 1);

    // Verify that the prepare entry has the oldest timestamp of any active transaction
    // in the transactions table.
    verifyOldestActiveTransactionTimestamp(
        testDB, opTimeBeforePrepareForCommit.ts, prepareTimestampForCommit);

    // Verify the total prepared and committed transaction counters are updated after a commit
    // and that the current prepared counter is decremented.
    PrepareHelpers.commitTransaction(session, prepareTimestampForCommit);
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPreparedThenCommitted", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentPrepared", 0);

    // Verify that other prepared transaction metrics have not changed.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPreparedThenAborted", 0);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPrepared", 1);

    // Test server metrics for a prepared transaction that is aborted.
    jsTest.log("Prepare a transaction and then abort it");

    const doc2 = {_id: 2, x: 2};

    // Start transaction and prepare transaction.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc2));

    const opTimeBeforePrepareForAbort = getLastOpTime(primary);
    const prepareTimestampForAbort = PrepareHelpers.prepareTransaction(session);

    // Verify that the total and current prepared counter is updated and the oldest active oplog
    // entry timestamp is shown.
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPrepared", 2);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentPrepared", 1);

    // Verify that the prepare entry has the oldest timestamp of any active transaction
    // in the transactions table.
    verifyOldestActiveTransactionTimestamp(
        testDB, opTimeBeforePrepareForAbort.ts, prepareTimestampForAbort);

    // Verify the total prepared and aborted transaction counters are updated after an abort and the
    // current prepared counter is decremented.
    assert.commandWorked(session.abortTransaction_forTesting());
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(newStatus);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPreparedThenAborted", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "currentPrepared", 0);

    // Verify that other prepared transaction metrics have not changed.
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPreparedThenCommitted", 1);
    verifyServerStatusChange(
        initialStatus.transactions, newStatus.transactions, "totalPrepared", 2);

    // End the session and stop the replica set.
    session.endSession();
    rst.stopSet();
}());
