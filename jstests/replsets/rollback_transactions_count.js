/**
 * Tests that rollback fixes the transactions table and gets the correct fastcounts for
 * transactions.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    const testName = "rollback_transactions_count";
    const dbName = testName;
    const collName = "txnCollName";

    const rollbackTest = new RollbackTest(testName);
    const primary = rollbackTest.getPrimary();

    const session1 = primary.startSession();
    const sessionDb1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDb1[collName];
    assert.commandWorked(sessionColl1.insert({a: 1}));
    session1.startTransaction();
    assert.commandWorked(sessionColl1.insert({b: 1}));
    session1.commitTransaction();

    rollbackTest.getTestFixture().awaitLastOpCommitted();
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));

    const session2 = primary.startSession();
    const sessionDb2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDb2[collName];
    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({c: 1}));
    session2.commitTransaction();

    rollbackTest.transitionToRollbackOperations();

    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({d: 1}));
    session2.commitTransaction();

    const session3 = primary.startSession();
    const sessionDb3 = session3.getDatabase(dbName);
    const sessionColl3 = sessionDb3[collName];
    session3.startTransaction();
    assert.commandWorked(sessionColl3.insert({e: 1}));
    session3.commitTransaction();

    assert.eq(sessionColl1.find().itcount(), 5);

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    try {
        rollbackTest.transitionToSteadyStateOperations();
    } finally {
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'}));
    }

    assert.eq(sessionColl1.find().itcount(), 3);
    assert.eq(primary.getDB('config')['transactions'].find().itcount(), 2);

    rollbackTest.stop();
})();
