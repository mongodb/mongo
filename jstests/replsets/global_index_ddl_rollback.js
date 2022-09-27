/**
 * Tests that global index container ddl operations can be rolled back.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/replsets/libs/rollback_test.js');

function uuidToNss(uuid) {
    const [_, uuidString] = uuid.toString().match(/"((?:\\.|[^"\\])*)"/);
    return "globalIndexes." + uuidString;
}

const rollbackTest = new RollbackTest(jsTestName());

const primary = rollbackTest.getPrimary();
const adminDB = primary.getDB("admin");
const globalIndexCreateUUID = UUID();
const globalIndexDropUUID = UUID();

// Create a global index container to be dropped.
assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexDropUUID}));

rollbackTest.transitionToRollbackOperations();

// Create a global index container to be rolled back.
assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexCreateUUID}));
// Drop a global index container, operation should be rolled back.
assert.commandWorked(adminDB.runCommand({_shardsvrDropGlobalIndex: globalIndexDropUUID}));

// Perform the rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.getTestFixture().nodes.forEach(function(node) {
    const nodeDB = node.getDB("system");

    // Check globalIndexCreateUUID creation is rolled back and does not exist.
    var res =
        nodeDB.runCommand({listCollections: 1, filter: {name: uuidToNss(globalIndexCreateUUID)}});
    assert.eq(res.cursor.firstBatch.length, 0);

    // Check globalIndexDropUUID drop is rolled back and still exists.
    res = nodeDB.runCommand({listCollections: 1, filter: {name: uuidToNss(globalIndexDropUUID)}});
    assert.eq(res.cursor.firstBatch.length, 1);
});

rollbackTest.stop();
})();
