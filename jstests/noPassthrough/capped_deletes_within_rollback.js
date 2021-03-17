/**
 * Tests that capped deletes occur during rollback on documents inserted earlier in rollback.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/replsets/libs/rollback_test.js');

const rollbackTest = new RollbackTest(jsTestName());

const testDB = function() {
    return rollbackTest.getPrimary().getDB('test');
};

const coll = function() {
    return testDB().getCollection(jsTestName());
};

assert.commandWorked(
    testDB().createCollection(coll().getName(), {capped: true, size: 100, max: 1}));
assert.commandWorked(coll().insert({a: 1}));

rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

assert.commandWorked(coll().insert([{b: 1}, {b: 2}]));

rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Stopping the test fixture runs validate with {enforceFastCount: true}. This will cause collection
// validation to fail if rollback did not perform capped deletes on documents that were inserted
// earlier in rollback.
rollbackTest.stop();
})();
