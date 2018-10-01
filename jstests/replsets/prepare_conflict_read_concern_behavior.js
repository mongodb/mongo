/**
 * Test calling reads with various read concerns on a prepared transaction. Snapshot, linearizable
 * and afterClusterTime reads are the only reads that should block on a prepared transaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const conn = replTest.getPrimary();

    const failureTimeout = 1 * 1000;       // 1 second.
    const successTimeout = 5 * 60 * 1000;  // 5 minutes.
    const dbName = "test";
    const collName = "prepare_conflict_read_concern_behavior";
    const collName2 = "prepare_conflict_read_concern_behavior2";
    const testDB = conn.getDB(dbName);
    const testColl = testDB.getCollection(collName);
    const testColl2 = testDB.getCollection(collName2);

    // Turn off timestamp reaping so that clusterTimeBeforePrepare doesn't get too old.
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
        mode: "alwaysOn",
    }));

    function runTest() {
        testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
        assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

        testDB.runCommand({drop: collName2, writeConcern: {w: "majority"}});
        assert.commandWorked(testDB.runCommand({create: collName2, writeConcern: {w: "majority"}}));

        const session = conn.startSession({causalConsistency: false});
        const sessionDB = session.getDatabase(dbName);
        const sessionColl = sessionDB.getCollection(collName);

        const read = function(read_concern, timeout, db, coll, num_expected) {
            let res = db.runCommand({
                find: coll,
                filter: {in_prepared_txn: 3},
                readConcern: read_concern,
                maxTimeMS: timeout,
            });

            if (num_expected) {
                assert(res.cursor, tojson(res));
                assert.eq(res.cursor.firstBatch.length, num_expected, tojson(res));
            }
            return res;
        };

        assert.commandWorked(
            testColl.insert({_id: 1, in_prepared_txn: 3}, {writeConcern: {w: "majority"}}));
        assert.commandWorked(testColl2.insert({_id: 1, in_prepared_txn: 3}));

        session.startTransaction();
        const clusterTimeBeforePrepare =
            assert.commandWorked(sessionColl.runCommand("insert", {documents: [{_id: 2}]}))
                .operationTime;
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        const clusterTimeAfterPrepare =
            assert
                .commandWorked(testColl.runCommand(
                    "insert",
                    {documents: [{_id: 3, in_prepared_txn: 3}], writeConcern: {w: "majority"}}))
                .operationTime;

        jsTestLog("prepareTimestamp: " + prepareTimestamp + " clusterTimeBeforePrepare: " +
                  clusterTimeBeforePrepare + " clusterTimeAfterPrepare: " +
                  clusterTimeAfterPrepare);

        assert.gt(prepareTimestamp, clusterTimeBeforePrepare);
        assert.gt(clusterTimeAfterPrepare, prepareTimestamp);

        jsTestLog(
            "Test read with read concern 'majority' doesn't block on a prepared transaction.");
        assert.commandWorked(read({level: 'majority'}, successTimeout, testDB, collName, 2));

        jsTestLog("Test read with read concern 'local' doesn't block on a prepared transaction.");
        assert.commandWorked(read({level: 'local'}, successTimeout, testDB, collName, 2));

        jsTestLog(
            "Test read with read concern 'available' doesn't block on a prepared transaction.");
        assert.commandWorked(read({level: 'available'}, successTimeout, testDB, collName, 2));

        jsTestLog("Test read with read concern 'linearizable' blocks on a prepared transaction.");
        assert.commandFailedWithCode(
            read({level: 'linearizable'}, failureTimeout, testDB, collName),
            ErrorCodes.MaxTimeMSExpired);

        // TODO SERVER-36953: uncomment this test
        // jsTestLog("Test afterClusterTime read before prepareTimestamp doesn't block on a " +
        //           "prepared transaction.");
        // assert.commandWorked(read({level: 'local', afterClusterTime: clusterTimeBeforePrepare},
        //                           successTimeout,
        //                           testDB,
        //                           collName,
        //                           2));

        jsTestLog("Test afterClusterTime read after prepareTimestamp blocks on a prepared " +
                  "transaction.");
        assert.commandFailedWithCode(
            read({level: 'local', afterClusterTime: clusterTimeAfterPrepare},
                 failureTimeout,
                 testDB,
                 collName),
            ErrorCodes.MaxTimeMSExpired);

        jsTestLog("Test read with afterClusterTime after prepareTimestamp on non-prepared " +
                  "documents doesn't block on a prepared transaction.");
        assert.commandWorked(read({level: 'local', afterClusterTime: clusterTimeAfterPrepare},
                                  successTimeout,
                                  testDB,
                                  collName2,
                                  1));

        // Create a second session and start a new transaction to test snapshot reads.
        const session2 = conn.startSession({causalConsistency: false});
        const sessionDB2 = session2.getDatabase(dbName);
        const sessionColl2 = sessionDB2.getCollection(collName);
        // This makes future reads in the transaction use a read timestamp after the
        // prepareTimestamp.
        session2.startTransaction(
            {readConcern: {level: "snapshot", atClusterTime: clusterTimeAfterPrepare}});

        jsTestLog("Test read with read concern 'snapshot' and a read timestamp after " +
                  "prepareTimestamp on non-prepared documents doesn't block on a prepared " +
                  "transaction.");
        assert.commandWorked(read({}, failureTimeout, sessionDB2, collName2, 1));

        jsTestLog("Test read with read concern 'snapshot' and a read timestamp after " +
                  "prepareTimestamp blocks on a prepared transaction.");
        assert.commandFailedWithCode(read({}, failureTimeout, sessionDB2, collName),
                                     ErrorCodes.MaxTimeMSExpired);

        session2.abortTransaction();
        session2.startTransaction(
            {readConcern: {level: "snapshot", atClusterTime: clusterTimeBeforePrepare}});

        jsTestLog("Test read with read concern 'snapshot' and atClusterTime before " +
                  "prepareTimestamp doesn't block on a prepared transaction.");
        assert.commandWorked(read({}, successTimeout, sessionDB2, collName, 1));

        session.abortTransaction();
        session.endSession();

        session2.abortTransaction();
        session2.endSession();
    }

    try {
        runTest();
    } finally {
        // Turn this failpoint off so that it doesn't impact other tests in the suite.
        assert.commandWorked(testDB.adminCommand({
            configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
            mode: "off",
        }));
    }

    replTest.stopSet();

}());