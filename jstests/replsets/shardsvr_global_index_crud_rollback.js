/**
 * Tests that global index key insert and delete are properly rolled back.
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

// We start on a clean slate: the global index container is empty.
assert.eq(0, primary.getDB("system").getCollection(collName).find().itcount());

const docKeyToInsert = {
    sk: 1,
    _id: 1
};
const docKeyToDelete = {
    sk: 1,
    _id: 5
};
// Add key to delete during rollback ops phase.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 5}, docKey: docKeyToDelete}));
    session.commitTransaction();
    session.endSession();
}

rollbackTest.transitionToRollbackOperations();

// Insert an index key to be rolled back.
{
    const session = primary.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrInsertGlobalIndexKey": globalIndexUUID, key: {a: 1}, docKey: docKeyToInsert}));
    assert.commandWorked(session.getDatabase("system").runCommand(
        {"_shardsvrDeleteGlobalIndexKey": globalIndexUUID, key: {a: 5}, docKey: docKeyToDelete}));
    session.commitTransaction();
    session.endSession();
}

// The inserted index key is present on the primary.
assert.eq(1, primary.getDB("system").getCollection(collName).find({_id: docKeyToInsert}).itcount());
// The deleted index key is not present on the primary.
assert.eq(0, primary.getDB("system").getCollection(collName).find({_id: docKeyToDelete}).itcount());

// Perform the rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Verify both global index key insert and delete have been rolled back. The container has exactly
// one entry, and is the one inserted before transitionToRollbackOperations.
rollbackTest.getTestFixture().nodes.forEach(function(node) {
    const nodeDB = node.getDB("system");
    const found = nodeDB.getCollection(collName).find();
    const elArr = found.toArray();
    assert.eq(1, elArr.length);
    assert.eq(elArr[0]["_id"], docKeyToDelete);
});

// TODO (SERVER-69847): fast count is not updated properly for global index CRUD ops after rollback.
// Current test implementation makes fastcount valid due to rolling back both a delete and an
// insert. After fixing fast count, we should make this test fail if fast count is not working
// properly.
// TODO (SERVER-69847): add a rollback test for _shardsvrWriteGlobalIndexKeys too.

rollbackTest.stop();
})();
