/**
 * Test that rollback can successfully roll back a committed and aborted prepared transaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const dbName = "test";
    const collName = "rollback_committed_aborted_prepared_transactions";

    const rollbackTest = new RollbackTest(dbName);
    let primary = rollbackTest.getPrimary();

    // Create collection we're using beforehand.
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    testDB.runCommand({drop: collName});
    assert.commandWorked(testDB.runCommand({create: collName}));

    // Start two different sessions on the primary.
    let session1 = primary.startSession({causalConsistency: false});
    const sessionID = session1.getSessionId();
    const session2 = primary.startSession({causalConsistency: false});

    let sessionDB1 = session1.getDatabase(dbName);
    let sessionColl1 = sessionDB1.getCollection(collName);

    const sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);

    // The following transactions will be rolled back.
    rollbackTest.transitionToRollbackOperations();

    // Prepare a transaction on the first session.
    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 1}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session1);

    // Prepare another transaction on the second session.
    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({_id: 2}));
    const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2);

    // Commit the first transaction.
    PrepareHelpers.commitTransactionAfterPrepareTS(session1, prepareTimestamp);

    // Abort the second transaction.
    session2.abortTransaction_forTesting();

    // Check that we have two transactions in the transactions table.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 2);

    // Both transactions should be rolled back.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    rollbackTest.transitionToSteadyStateOperations();

    // Make sure there are no transactions in the transactions table.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 0);

    // Make sure the first collection is empty since the insert should have been rolled back.
    assert.eq(sessionColl1.find().itcount(), 0);

    // Get the correct members after the toplogy changes.
    primary = rollbackTest.getPrimary();
    const rst = rollbackTest.getTestFixture();
    const secondaries = rst.getSecondaries();

    // Make sure we can successfully run a prepared transaction on the same first session after
    // going through rollback.
    session1 =
        PrepareHelpers.createSessionWithGivenId(primary, sessionID, {causalConsistency: false});
    sessionDB1 = session1.getDatabase(dbName);
    sessionColl1 = sessionDB1.getCollection(collName);

    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 3}));
    const prepareTimestamp3 = PrepareHelpers.prepareTransaction(session1);
    PrepareHelpers.commitTransactionAfterPrepareTS(session1, prepareTimestamp3);

    rollbackTest.stop();
}());
