/**
 * Tests that global index key insert is properly rolled back.
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

const rollbackTest = new RollbackTest(jsTestName());

const primary = rollbackTest.getPrimary();
const adminDB = primary.getDB("admin");
const globalIndexUUID = UUID();
const [_, uuidString] = globalIndexUUID.toString().match(/"((?:\\.|[^"\\])*)"/);
const collName = "globalIndexes." + uuidString;

assert.commandWorked(adminDB.runCommand({_shardsvrCreateGlobalIndex: globalIndexUUID}));

rollbackTest.transitionToRollbackOperations();

// We start on a clean slate: the global index container is empty.
assert.eq(0, primary.getDB("system").getCollection(collName).find().itcount());

// Insert an index key to be rolled back.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: {sk: 1, _id: 1}}));
    session.commitTransaction();
    session.endSession();
}

// The index key is present on the primary.
assert.eq(1, primary.getDB("system").getCollection(collName).find().itcount());

// Perform the rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Verify the index key has been rolled back: the container is empty again.
rollbackTest.getTestFixture().nodes.forEach(function(node) {
    const nodeDB = node.getDB("system");
    assert.eq(0, nodeDB.getCollection(collName).find().itcount());
});

rollbackTest.stop();
})();
