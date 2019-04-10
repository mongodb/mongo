/**
 * Tests that rolling back prepared transactions does not lead to prepare conflicts. These may
 * occur while writing out rollback data files for ops that were done outside of those
 * transactions but are in conflict with their contents. However, since we abort prepared
 * transactions on rollback, such prepare conflicts would be unnecessary. This test therefore
 * verifies that they do not happen.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const name = "rollback_files_no_prepare_conflicts";
    const dbName = "test";
    const collName = name;

    const rollbackTest = new RollbackTest(name);

    let primary = rollbackTest.getPrimary();
    let testDB = primary.getDB(dbName);
    let testColl = testDB.getCollection(collName);

    jsTestLog("Issue an insert that will be common to both nodes.");
    assert.commandWorked(testColl.insert({_id: 42, a: "one"}));

    rollbackTest.transitionToRollbackOperations();

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    jsTestLog("Make an update to that document outside of a transaction on the rollback node.");
    assert.commandWorked(testColl.update({_id: 42, a: "one"}, {_id: 42, a: "two"}));

    session.startTransaction();

    jsTestLog("Update the same document on the same node, this time as part of a transaction.");
    assert.commandWorked(sessionColl.update({_id: 42, a: "two"}, {_id: 42, a: "three"}));

    jsTestLog("Prepare the transaction on the rollback node.");
    PrepareHelpers.prepareTransaction(session, {w: 1});

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    jsTestLog("Verify that the document is in the same state as it was at the common point.");
    primary = rollbackTest.getPrimary();
    testDB = primary.getDB(dbName);
    testColl = testDB.getCollection(collName);
    assert.docEq(testColl.findOne({_id: 42}), {_id: 42, a: "one"});

    rollbackTest.stop();
})();
