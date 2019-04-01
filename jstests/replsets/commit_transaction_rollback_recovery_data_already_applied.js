/**
 * Tests that rollback recovery successfully replays the commitTransaction oplog entry, but does
 * not re-apply the operations from the transaction if the data already reflects the transaction.
 * If the operations are replayed, this will cause a BSONTooLarge exception.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const dbName = "test";
    const collName = "commit_transaction_rollback_recovery_data_already_applied";

    const rollbackTest = new RollbackTest(dbName);
    let primary = rollbackTest.getPrimary();

    // Create collection we're using beforehand.
    let testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    assert.commandWorked(testDB.runCommand({create: collName}));

    let session = primary.startSession({causalConsistency: false});
    let sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // Construct a large array such that two arrays in the same document are not greater than the
    // 16MB limit, but that three such arrays in the same document are greater than 16MB. This will
    // be helpful in recreating an idempotency issue that exists when applying the operations from
    // a transaction after the data already reflects the transaction.
    const largeArray = new Array(7 * 1024 * 1024).join('x');
    assert.commandWorked(testColl.insert({_id: 1, "a": largeArray}));

    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {"b": largeArray}}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$unset: {"b": 1}}));
    assert.commandWorked(sessionColl.update({_id: 1}, {$set: {"c": largeArray}}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    const commitTimestamp =
        assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 2}]})).operationTime;
    const recoveryTimestamp =
        assert.commandWorked(testColl.runCommand("insert", {documents: [{_id: 3}]})).operationTime;

    jsTestLog("Holding back the stable timestamp to right after the commitTimestamp");

    // Hold back the stable timestamp to be right after the commitTimestamp, but before the
    // commitTransaction oplog entry so that the data will reflect the transaction during recovery.
    assert.commandWorked(testDB.adminCommand({
        "configureFailPoint": 'holdStableTimestampAtSpecificTimestamp',
        "mode": 'alwaysOn',
        "data": {"timestamp": recoveryTimestamp}
    }));

    jsTestLog("Committing the transaction");

    // Since the commitTimestamp is after the last snapshot, this oplog entry will be replayed
    // during replication recovery during rollback.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, commitTimestamp));

    // During rollback, we will replay the commit oplog entry but should not re-apply the
    // operations.
    rollbackTest.transitionToRollbackOperations();
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    try {
        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    } finally {
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: 'holdStableTimestampAtSpecificTimestamp', mode: 'off'}));
    }

    rollbackTest.transitionToSteadyStateOperations();

    primary = rollbackTest.getPrimary();
    testDB = primary.getDB(dbName);
    session = primary.startSession({causalConsistency: false});
    sessionDB = session.getDatabase(dbName);
    session.startTransaction();

    // Make sure that the data reflects all the operations from the transaction after recovery.
    const res = testDB[collName].findOne({_id: 1});
    assert.eq(res, {_id: 1, "a": largeArray, "c": largeArray}, res);

    // Make sure that another write on the same document from the transaction has no write conflict.
    // Also, make sure that we can run another transaction after recovery without any problems.
    assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 1}));
    prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 1});

    rollbackTest.stop();

}());
