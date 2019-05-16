/**
 * 1. Test that rollback can successfully recover an aborted prepared transaction that was
 * rolled-back but was in prepare between the stable timestamp and the common point.
 * 2. Test that rollback can successfully recover a committed prepared transaction that was
 * rolled-back but was in prepare before the stable timestamp.
 *
 * This test holds back the stable timestamp and starts two prepared transactions
 * before transitioning to rollback operations, where the branches of history on
 * the rollback node and sync source will diverge. This ensures that we prepare
 * the transactions in between the stable timestamp and the common point.
 *
 * After a rollback of commit/abort, we should correctly reconstruct the two prepared transactions
 * and be able to commit/abort them again.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const dbName = "test";
    const collName = "recover_prepared_transaction_state_after_rollback";

    const rollbackTest =
        new RollbackTest(dbName, undefined, true /* expect prepared transaction after rollback */);
    let primary = rollbackTest.getPrimary();

    // Create collection we're using beforehand.
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    assert.commandWorked(testDB.runCommand({create: collName}));

    // Start two different sessions on the primary.
    let session1 = primary.startSession({causalConsistency: false});
    let session2 = primary.startSession({causalConsistency: false});

    // Save both session IDs so we can later start sessions with the same IDs and commit or
    // abort a prepared transaction on them.
    const sessionID1 = session1.getSessionId();
    const sessionID2 = session2.getSessionId();

    let sessionDB1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDB1.getCollection(collName);

    let sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);

    assert.commandWorked(sessionColl1.insert({_id: 1}));
    assert.commandWorked(sessionColl1.insert({_id: 2}));

    rollbackTest.awaitLastOpCommitted();

    // Prepare a transaction on the first session whose commit will be rolled-back.
    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({_id: 3}));
    assert.commandWorked(sessionColl1.update({_id: 1}, {$set: {a: 1}}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session1);

    // Prevent the stable timestamp from moving beyond the following prepared transactions so
    // that when we replay the oplog from the stable timestamp, we correctly recover them.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));

    // The following transactions will be prepared before the common point, so they must be in
    // prepare after rollback recovery.

    // Prepare another transaction on the second session whose abort will be rolled-back.
    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({_id: 4}));
    assert.commandWorked(sessionColl2.update({_id: 2}, {$set: {b: 2}}));
    const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2, {w: 1});

    // Check that we have two transactions in the transactions table.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 2);

    // This characterizes the current behavior of fastcount, which is that the two open transaction
    // count toward the value.
    assert.eq(testColl.count(), 4);

    // The following commit and abort will be rolled back.
    rollbackTest.transitionToRollbackOperations();
    PrepareHelpers.commitTransaction(session1, prepareTimestamp);
    session2.abortTransaction_forTesting();

    // The fastcount should be accurate because there are no open transactions.
    assert.eq(testColl.count(), 3);

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    try {
        rollbackTest.transitionToSteadyStateOperations();
    } finally {
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'}));
    }

    // Make sure there are two transactions in the transactions table after rollback recovery.
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 2);

    // Make sure we can only see the first write and cannot see the writes from the prepared
    // transactions or the write that was rolled back.
    arrayEq(sessionColl1.find().toArray(), [{_id: 1}, {_id: 2}]);
    arrayEq(testColl.find().toArray(), [{_id: 1}, {_id: 2}]);

    // This check characterizes the current behavior of fastcount after rollback. It will not be
    // correct, but reflects the count at the point where both transactions are not yet committed or
    // aborted (because the operations were not majority committed). The count will eventually be
    // correct once the commit and abort are retried.
    assert.eq(sessionColl1.count(), 4);
    assert.eq(testColl.count(), 4);

    // Get the correct primary after the topology changes.
    primary = rollbackTest.getPrimary();
    rollbackTest.awaitReplication();

    // Make sure we can successfully commit the first rolled back prepared transaction.
    session1 =
        PrepareHelpers.createSessionWithGivenId(primary, sessionID1, {causalConsistency: false});
    sessionDB1 = session1.getDatabase(dbName);
    // The next transaction on this session should have a txnNumber of 0. We explicitly set this
    // since createSessionWithGivenId does not restore the current txnNumber in the shell.
    session1.setTxnNumber_forTesting(0);
    const txnNumber1 = session1.getTxnNumber_forTesting();

    // Make sure we cannot add any operations to a prepared transaction.
    assert.commandFailedWithCode(sessionDB1.runCommand({
        insert: collName,
        txnNumber: NumberLong(txnNumber1),
        documents: [{_id: 10}],
        autocommit: false,
    }),
                                 ErrorCodes.PreparedTransactionInProgress);

    // Make sure that writing to a document that was updated in the first prepared transaction
    // causes a write conflict.
    assert.commandFailedWithCode(
        sessionDB1.runCommand(
            {update: collName, updates: [{q: {_id: 1}, u: {$set: {a: 2}}}], maxTimeMS: 5 * 1000}),
        ErrorCodes.MaxTimeMSExpired);

    const commitTimestamp = Timestamp(prepareTimestamp.getTime(), prepareTimestamp.getInc() + 1);
    assert.commandWorked(sessionDB1.adminCommand({
        commitTransaction: 1,
        commitTimestamp: commitTimestamp,
        txnNumber: NumberLong(txnNumber1),
        autocommit: false,
    }));
    // Retry the commitTransaction command after rollback.
    assert.commandWorked(sessionDB1.adminCommand({
        commitTransaction: 1,
        commitTimestamp: commitTimestamp,
        txnNumber: NumberLong(txnNumber1),
        autocommit: false,
    }));

    // Make sure we can successfully abort the second recovered prepared transaction.
    session2 =
        PrepareHelpers.createSessionWithGivenId(primary, sessionID2, {causalConsistency: false});
    sessionDB2 = session2.getDatabase(dbName);
    // The next transaction on this session should have a txnNumber of 0. We explicitly set this
    // since createSessionWithGivenId does not restore the current txnNumber in the shell.
    session2.setTxnNumber_forTesting(0);
    const txnNumber2 = session2.getTxnNumber_forTesting();

    // Make sure we cannot add any operations to a prepared transaction.
    assert.commandFailedWithCode(sessionDB2.runCommand({
        insert: collName,
        txnNumber: NumberLong(txnNumber2),
        documents: [{_id: 10}],
        autocommit: false,
    }),
                                 ErrorCodes.PreparedTransactionInProgress);

    // Make sure that writing to a document that was updated in the second prepared transaction
    // causes a write conflict.
    assert.commandFailedWithCode(
        sessionDB2.runCommand(
            {update: collName, updates: [{q: {_id: 2}, u: {$set: {b: 3}}}], maxTimeMS: 5 * 1000}),
        ErrorCodes.MaxTimeMSExpired);

    assert.commandWorked(sessionDB2.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber2),
        autocommit: false,
    }));

    rollbackTest.awaitReplication();

    // Make sure we can see the result of the committed prepared transaction and cannot see the
    // write from the aborted transaction.
    arrayEq(testColl.find().toArray(), [{_id: 1, a: 1}, {_id: 2}, {_id: 3}]);
    assert.eq(testColl.count(), 3);

    rollbackTest.stop();

}());
