/**
 * 1. Test that rollback can successfully recover committed prepared transactions that were prepared
 * before the stable timestamp and committed between the stable timestamp and the common point.
 * 2. Test that rollback can successfully recover aborted prepared transactions that were prepared
 * and aborted between the stable timestamp and the common point.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const dbName = "test";
    const collName = "recover_committed_aborted_prepared_transactions";

    const rollbackTest = new RollbackTest(dbName);
    let primary = rollbackTest.getPrimary();

    // Create collection we're using beforehand.
    let testDB = primary.getDB(dbName);
    let testColl = testDB.getCollection(collName);

    assert.commandWorked(testDB.runCommand({create: collName}));

    // Start two different sessions on the primary.
    let session1 = primary.startSession({causalConsistency: false});
    const sessionID1 = session1.getSessionId();
    const session2 = primary.startSession({causalConsistency: false});

    let sessionDB1 = session1.getDatabase(dbName);
    let sessionColl1 = sessionDB1.getCollection(collName);

    const sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);

    assert.commandWorked(sessionColl1.insert({id: 1}));

    rollbackTest.awaitLastOpCommitted();

    // Prepare a transaction on the first session which will be committed eventually.
    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({id: 2}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session1);

    // Prevent the stable timestamp from moving beyond the following prepared transactions so
    // that when we replay the oplog from the stable timestamp, we correctly recover them.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));

    // The following transactions will be prepared before the common point, so they must be in
    // prepare after rollback recovery.

    // Prepare another transaction on the second session which will be aborted.
    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({id: 3}));
    const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2, {w: 1});

    // Commit the first transaction.
    assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestamp));

    // Abort the second transaction.
    session2.abortTransaction_forTesting();

    // Check that we have two transactions in the transactions table.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 2);

    // The following write will be rolled back.
    rollbackTest.transitionToRollbackOperations();
    assert.commandWorked(testColl.insert({id: 4}));

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    try {
        rollbackTest.transitionToSteadyStateOperations();
    } finally {
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'}));
    }

    // Make sure there are two transactions in the transactions table.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 2);

    // Make sure we can see the first two writes and the insert from the first prepared transaction.
    // Make sure we cannot see the insert from the second prepared transaction or the writes after
    // transitionToRollbackOperations.
    arrayEq(testColl.find().toArray(), [{_id: 1}, {_id: 2}]);
    arrayEq(sessionColl1.find().toArray(), [{_id: 1}, {_id: 2}]);

    assert.eq(testColl.count(), 2);
    assert.eq(sessionColl1.count(), 2);

    // Get the correct members after the topology changes.
    primary = rollbackTest.getPrimary();
    testDB = primary.getDB(dbName);
    testColl = testDB.getCollection(collName);
    const rst = rollbackTest.getTestFixture();
    const secondaries = rst.getSecondaries();

    // Make sure we can successfully run a prepared transaction on the same first session after
    // going through rollback. This ensures that the session state has properly been restored.
    session1 =
        PrepareHelpers.createSessionWithGivenId(primary, sessionID1, {causalConsistency: false});
    sessionDB1 = session1.getDatabase(dbName);
    sessionColl1 = sessionDB1.getCollection(collName);
    // The next transaction on this session should have a txnNumber of 1. We explicitly set this
    // since createSessionWithGivenId does not restore the current txnNumber in the shell.
    session1.setTxnNumber_forTesting(1);

    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 5}));
    const prepareTimestamp3 = PrepareHelpers.prepareTransaction(session1);
    // Make sure we can successfully retry the commitTransaction command after rollback.
    assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestamp3));

    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 6}));
    PrepareHelpers.prepareTransaction(session1);
    session1.abortTransaction_forTesting();
    // Retrying the abortTransaction command should fail with a NoSuchTransaction error.
    assert.commandFailedWithCode(sessionDB1.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(session1.getTxnNumber_forTesting()),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);

    // Make sure we can see the insert after committing the prepared transaction.
    arrayEq(testColl.find().toArray(), [{_id: 1}, {_id: 2}, {_id: 5}]);
    assert.eq(testColl.count(), 3);

    rollbackTest.stop();
}());
