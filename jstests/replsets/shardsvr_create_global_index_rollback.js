/**
 * Tests that global index container creation is properly rolled back.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_61,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/replsets/libs/rollback_test.js');

const rollbackTest = new RollbackTest(jsTestName());

const primary = rollbackTest.getPrimary();
const adminDB = primary.getDB("admin");
const globalIndexUUID = UUID();

rollbackTest.transitionToRollbackOperations();

// Create a global index container to be rolled back.
assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));

// Perform the rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

const [_, uuidString] = globalIndexUUID.toString().match(/"((?:\\.|[^"\\])*)"/);
const namespace = "globalIndexes." + uuidString;

// Check the global index container collection does not exist.
rollbackTest.getTestFixture().nodes.forEach(function(node) {
    const nodeDB = node.getDB("system");
    const res = nodeDB.runCommand({listCollections: 1, filter: {name: namespace}});
    assert.eq(res.cursor.firstBatch.length, 0);
});

rollbackTest.stop();
})();
