/**
 * Tests that transaction CRUD operations against a shard on a capped collection fail.
 *
 * Capped collection writes take collection X locks and cannot guarantee successful application
 * across all replica set members (a write could succeed on a primary and fail on one of the
 * secondaries), so they are not allowed in prepared transactions or transactions on shards more
 * generally.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    const st = new ShardingTest({shards: 1});

    const dbName = "test_db";
    const cappedCollName = "test_capped_coll";
    const testDB = st.shard0.getDB('test_db');

    jsTest.log("Creating a capped collection '" + dbName + "." + cappedCollName + "'.");
    assert.commandWorked(testDB.createCollection(cappedCollName, {capped: true, size: 500}));

    jsTest.log("Adding data to the capped collection so that the update op can be tested in " +
               "the subsequent transaction attempts");
    const testCappedColl = testDB.getCollection(cappedCollName);
    const testDocument = {"a": "docToTryToUpdateThenDelete"};
    assert.writeOK(testCappedColl.insert(testDocument));

    jsTest.log("Setting up a session in which to execute transaction ops.");
    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionCappedColl = sessionDB.getCollection(cappedCollName);

    jsTest.log("Starting a transaction for an insert op against a capped collection that should " +
               "fail");
    session.startTransaction();
    assert.commandFailedWithCode(sessionCappedColl.insert({"x": 55}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTest.log("Starting a transaction for an update op against a capped collection that should " +
               "fail");
    session.startTransaction();
    assert.commandFailedWithCode(sessionCappedColl.update(testDocument, {"a": 1000}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Deletes do not work against capped collections so we will not test it in a transaction.

    session.endSession();

    st.stop();
})();
