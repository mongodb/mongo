/**
 * Tests that capped collections get the correct fast counts after rollback in FCV 4.4.
 */
(function() {
'use strict';

load("jstests/multiVersion/libs/multiversion_rollback.js");

const testName = 'rollback_capped_deletions';
const dbName = testName;
const collName = 'cappedCollName';

const replSet = setupReplicaSet(testName, 'latest', 'last-lts');
const rollbackTest = new RollbackTest(testName, replSet);
const primary = rollbackTest.getPrimary();
const testDb = primary.getDB(dbName);

assert.commandWorked(testDb.runCommand({
    'create': collName,
    'capped': true,
    'size': 40,
}));
const coll = testDb.getCollection(collName);
assert.commandWorked(coll.insert({a: 1}));

rollbackTest.awaitLastOpCommitted();
assert.commandWorked(
    primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'alwaysOn'}));

assert.commandWorked(coll.insert({bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb: 1}));
assert.commandWorked(coll.insert({cccccccccccccccccccccccccccccccccccccccccccc: 1}));
assert.commandWorked(coll.insert({dddddddddddddddddddddddddddddddddddddddddddd: 1}));
assert.commandWorked(coll.insert({eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee: 1}));

rollbackTest.transitionToRollbackOperations();

assert.commandWorked(coll.insert({ffffffffffffffffffffffffffffffffffffffffffff: 1}));

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
try {
    rollbackTest.transitionToSteadyStateOperations();
} finally {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'disableSnapshotting', mode: 'off'}));
}

// The fast count checks occur when tearing down the fixture as part of the consistency checks.
rollbackTest.stop();
})();
