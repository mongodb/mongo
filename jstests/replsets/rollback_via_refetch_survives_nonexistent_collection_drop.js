/**
 * Test that rollbackViaRefetch survives an attempt to drop a collection that does not exist.
 * This test simulates a scenario where a collection is dropped during the first rollback
 * attempt.
 *
 * We use a failpoint to ensure the collection was dropped before forcing rollback to return
 * early with a recoverable error. We then turn this failpoint off so that the second attempt
 * can succeed even though the collection has already been dropped.
 *
 */

(function() {
"use strict";
load("jstests/libs/check_log.js");
load("jstests/replsets/libs/rollback_test.js");

const dbName = "test";
const collName = "rollback_via_refetch_survives_nonexistent_collection_drop";

// Provide RollbackTest with custom ReplSetTest so we can set enableMajorityReadConcern.
const rst = new ReplSetTest(
    {name: collName, nodes: 3, useBridge: true, nodeOptions: {enableMajorityReadConcern: "false"}});

rst.startSet();
const config = rst.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
rst.initiate(config);

const rollbackTest = new RollbackTest(collName, rst);

// Stop replication from the current primary, the rollback node.
const rollbackNode = rollbackTest.transitionToRollbackOperations();
const rollbackDB = rollbackNode.getDB(dbName);

jsTestLog("Turning on the rollbackExitEarlyAfterCollectionDrop fail point");
assert.commandWorked(rollbackDB.adminCommand(
    {configureFailPoint: 'rollbackExitEarlyAfterCollectionDrop', mode: 'alwaysOn'}));

// Create a collection on the rollback node.
assert.commandWorked(rollbackDB.runCommand({create: collName}));

// Step down the current primary and elect the node that does not have the collection.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

jsTestLog("Attempting to roll back.");
// Make the old primary rollback against the new primary. This attempt should fail because the
// rollbackExitEarlyAfterCollectionDrop fail point is set. We fail with a recoverable error
// so that the rollback will be retried.
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// Make sure we exit the rollback early by checking for the correct log messages.
checkLog.contains(rollbackDB.getMongo(),
                  "rollbackExitEarlyAfterCollectionDrop fail point enabled.");

jsTestLog("Turning off the rollbackExitEarlyAfterCollectionDrop fail point");
// A rollback attempt after turning off the fail point should succeed even if we already
// dropped the collection.
assert.commandWorked(rollbackDB.adminCommand(
    {configureFailPoint: 'rollbackExitEarlyAfterCollectionDrop', mode: 'off'}));

rollbackTest.transitionToSteadyStateOperations();

// After a successful rollback attempt, we should have seen the following log message to ensure
// that we tried to drop a non-existent collection and continued without acquiring a database
// lock.
checkLog.contains(rollbackDB.getMongo(), "This collection does not exist");

rollbackTest.stop();
}());