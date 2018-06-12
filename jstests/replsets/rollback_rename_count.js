/**
 * Tests that rollback corrects fastcounts even when collections are renamed.
 */
(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    const testName = "rollback_rename_count";
    const dbName = testName;

    const rollbackTest = new RollbackTest(testName);
    const primary = rollbackTest.getPrimary();
    const testDb = primary.getDB(dbName);

    // This collection is non-empty at the stable timestamp.
    const fromCollName1 = "fromCollName1";
    const toCollName1 = "toCollName1";
    let coll1 = testDb.getCollection(fromCollName1);
    assert.commandWorked(coll1.insert({a: 1}));

    rollbackTest.getTestFixture().awaitLastOpCommitted();
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));

    assert.commandWorked(coll1.renameCollection(toCollName1));
    coll1 = testDb.getCollection(toCollName1);
    assert.commandWorked(coll1.insert({b: 1}));

    // This collection is empty at the stable timestamp.
    const fromCollName2 = "fromCollName2";
    const toCollName2 = "toCollName2";
    let coll2 = testDb.getCollection(fromCollName2);
    assert.commandWorked(coll2.insert({c: 1}));
    assert.commandWorked(coll2.renameCollection(toCollName2));
    coll2 = testDb.getCollection(toCollName2);
    assert.commandWorked(coll2.insert({d: 1}));

    rollbackTest.transitionToRollbackOperations();

    assert.commandWorked(coll1.insert({e: 1}));

    assert.eq(coll1.find().itcount(), 3);
    assert.eq(coll2.find().itcount(), 2);

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    try {
        rollbackTest.transitionToSteadyStateOperations();
    } finally {
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'}));
    }

    assert.eq(coll1.find().itcount(), 2);
    assert.eq(coll2.find().itcount(), 2);

    rollbackTest.stop();
})();
