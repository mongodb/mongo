/**
 * Test that a dropIndex on a collection that was renamed is rolled back successfully.
 *
 * This test works by creating a collection and an index in that collection,
 * then renaming that collection and rolling back a drop on that index.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");

    const testName = "rollback_drop_index_after_rename";
    const dbName = testName;

    var fromColl = "fromColl";
    var toColl = "toColl";
    var idxName = "a_1";

    // Operations that will be present on both nodes, before the common point.
    let CommonOps = (node) => {
        let testDb = node.getDB(dbName);
        // This creates the collection implicitly and then creates the index.
        assert.commandWorked(testDb.runCommand({
            createIndexes: fromColl,
            indexes: [{
                key: {
                    "a": 1,
                },
                name: idxName
            }]
        }));
    };

    // Operations that will be performed on the rollback node past the common point.
    let RollbackOps = (node) => {
        let testDb = node.getDB(dbName);
        assert.commandWorked(testDb.adminCommand({
            renameCollection: dbName + "." + fromColl,
            to: dbName + "." + toColl,
        }));
        assert.commandWorked(testDb.runCommand({dropIndexes: toColl, index: idxName}));
    };

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest(testName);
    CommonOps(rollbackTest.getPrimary());

    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    // Wait for rollback to finish.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    rollbackTest.stop();
})();
