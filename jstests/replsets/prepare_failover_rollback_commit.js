/**
 * This tests that preparing a transaction successfully will update the lastWriteOpTime on
 * secondaries so that the corresponding commitTransaction oplog entry has a correct prevOpTime.
 * The test exercises a failover right after a prepare, so that we have to commit the transaction
 * while talking to a node that was in secondary state when it was prepared. We commit that
 * transaction, then roll back that commit entry, the success of which depends on the prevOpTime
 * being set properly.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/replsets/libs/rollback_test.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "prepare_failover_rollback_commit";

    const rollbackTest = new RollbackTest(collName);

    let primary = rollbackTest.getPrimary();
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    // First create the collection for all.
    assert.commandWorked(testColl.insert({"a": "baseDoc"}));

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({"b": "transactionDoc"}));

    // Prepare a transaction. This will be replicated to the secondary.
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Do a failover first, without rolling back any of the data from this test. We want the
    // current secondary to become primary and inherit the prepared transaction.
    rollbackTest.transitionToRollbackOperations();
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

    // Now set up a rollback scenario for that new primary.
    rollbackTest.transitionToRollbackOperations();

    // Create a proxy session to reuse the session state of the old primary.
    primary = rollbackTest.getPrimary();
    const newSession1 = new _DelegatingDriverSession(primary, session);

    // Commit the transaction on this primary. We expect the commit to roll back.
    assert.commandWorked(PrepareHelpers.commitTransaction(newSession1, prepareTimestamp));

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

    // Create a proxy session to reuse the session state of the old primary.
    primary = rollbackTest.getPrimary();
    const newSession2 = new _DelegatingDriverSession(primary, session);

    // Commit the transaction for all to conclude the test.
    assert.commandWorked(PrepareHelpers.commitTransaction(newSession2, prepareTimestamp));

    rollbackTest.stop();
})();
