// Checks that the global histogram counter for transactions are updated as we expect.
// @tags: [requires_replication, uses_transactions]
(function() {
    "use strict";

    // Set up the replica set.
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // Set up the test database.
    const dbName = "test";
    const collName = "global_transaction_latency_histogram";

    const testDB = primary.getDB(dbName);
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Start the session.
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    function getHistogramStats() {
        return testDB.serverStatus({opLatencies: {histograms: 1}}).opLatencies;
    }

    // Checks that the actual value is within a minimum on the bound of the expected value. All
    // arguments must be in the same units.
    function assertLowerBound(expected, actual, bound) {
        assert.gte(actual, expected - bound);
    }

    // This function checks the diff between the last histogram and the current histogram, not the
    // absolute values.
    function checkHistogramDiff(lastHistogram, thisHistogram, fields) {
        for (let key in fields) {
            if (fields.hasOwnProperty(key)) {
                assert.eq(thisHistogram[key].ops - lastHistogram[key].ops, fields[key]);
            }
        }
        return thisHistogram;
    }

    // This function checks the diff between the last histogram's accumulated transactions latency
    // and this histogram's accumulated transactions latency is within a reasonable bound of what
    // we expect.
    function checkHistogramLatencyDiff(lastHistogram, thisHistogram, sleepTime) {
        let latencyDiff = thisHistogram.transactions.latency - lastHistogram.transactions.latency;
        // Check the bound in microseconds, which is the unit the latency is in. We do not check
        // upper bound because of unknown extra server latency.
        assertLowerBound(sleepTime * 1000, latencyDiff, 50000);
        return thisHistogram;
    }

    let lastHistogram = getHistogramStats();

    // Verify the base stats are correct.
    lastHistogram = checkHistogramDiff(lastHistogram,
                                       getHistogramStats(),
                                       {"reads": 0, "writes": 0, "commands": 1, "transactions": 0});

    // Test histogram increments on a successful transaction. "commitTransaction" and "serverStatus"
    // commands are counted towards the "commands" counter.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));
    session.commitTransaction();
    lastHistogram = checkHistogramDiff(lastHistogram,
                                       getHistogramStats(),
                                       {"reads": 0, "writes": 1, "commands": 2, "transactions": 1});

    // Test histogram increments on aborted transaction due to error (duplicate insert).
    session.startTransaction();
    assert.commandFailedWithCode(sessionColl.insert({_id: "insert-1"}), ErrorCodes.DuplicateKey);
    lastHistogram = checkHistogramDiff(lastHistogram,
                                       getHistogramStats(),
                                       {"reads": 0, "writes": 1, "commands": 1, "transactions": 1});

    // Ensure that the transaction was aborted on failure.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    lastHistogram = checkHistogramDiff(lastHistogram,
                                       getHistogramStats(),
                                       {"reads": 0, "writes": 0, "commands": 2, "transactions": 0});

    // Test histogram increments on an aborted transaction. "abortTransaction" command is counted
    // towards the "commands" counter.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-2"}));
    assert.commandWorked(session.abortTransaction_forTesting());
    lastHistogram = checkHistogramDiff(lastHistogram,
                                       getHistogramStats(),
                                       {"reads": 0, "writes": 1, "commands": 2, "transactions": 1});

    // Test histogram increments on a multi-statement committed transaction.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "insert-3"}));
    assert.commandWorked(sessionColl.insert({_id: "insert-4"}));
    assert.eq(sessionColl.find({_id: "insert-1"}).itcount(), 1);
    session.commitTransaction();
    lastHistogram = checkHistogramDiff(lastHistogram,
                                       getHistogramStats(),
                                       {"reads": 1, "writes": 2, "commands": 2, "transactions": 1});

    // Test that the cumulative transaction latency counter is updated appropriately after a
    // sequence of back-to-back 200 ms transactions.
    const sleepTime = 200;
    for (let i = 0; i < 3; i++) {
        session.startTransaction();
        assert.eq(sessionColl.find({_id: "insert-1"}).itcount(), 1);
        sleep(sleepTime);
        session.commitTransaction();
        lastHistogram = checkHistogramLatencyDiff(lastHistogram, getHistogramStats(), sleepTime);
    }

    session.endSession();
    rst.stopSet();
}());
