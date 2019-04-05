/**
 * Tests that a prepared transactions are correctly rolled-back.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const rollbackTest = new RollbackTest();
    const rollbackNode = rollbackTest.getPrimary();

    const testDB = rollbackNode.getDB("test");
    const collName = "rollback_prepare_transaction";
    const testColl = testDB.getCollection(collName);
    const txnDoc = {_id: 42};

    // We perform some operations on the collection aside from starting and preparing a transaction
    // in order to cause the count diff computed by replication to be non-zero.
    assert.commandWorked(testColl.insert({_id: 1}));

    rollbackTest.transitionToRollbackOperations();

    // The following operations will be rolled-back.
    assert.commandWorked(testColl.insert({_id: 2}));

    const session = rollbackNode.startSession();
    const sessionDB = session.getDatabase(testDB.getName());
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert(txnDoc));

    // Use w: 1 to simulate a prepare that will not become majority-committed.
    PrepareHelpers.prepareTransaction(session, {w: 1});

    // This is not exactly correct, but characterizes the current behavior of fastcount, which
    // includes the prepared but uncommitted transaction in the collection count.
    assert.eq(3, testColl.count());

    // Only two documents are visible.
    arrayEq([{_id: 1}, {_id: 2}], testColl.find().toArray());

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    // TODO: Workaround for SERVER-40322
    assert.commandWorked(
        rollbackNode.adminCommand({setParameter: 1, createRollbackDataFiles: false}));

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Both the regular insert and prepared insert should be rolled-back.
    assert.eq(1, testColl.count());
    assert.eq({_id: 1}, testColl.findOne());

    rollbackTest.stop();
})();
