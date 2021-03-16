/*
 * Test that a node in rollback state can safely be removed from the replica set
 * config via reconfig. See SERVER-48179.
 */
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

const dbName = "test";
const collName = "rollbackColl";

// RollbackTest begins with a 3 node replSet in steady-state replication.
let rollbackTest = new RollbackTest(jsTestName());
let rollbackNode = rollbackTest.getPrimary();
let secondTermPrimary = rollbackTest.getSecondary();
let tieBreakerNode = rollbackTest.getTieBreaker();

// Isolate the current primary from the replica sets. Ops applied here will get rolled back.
rollbackTest.transitionToRollbackOperations();
assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({"num1": 123}));

// Elect the previous secondary as the new primary.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
assert.commandWorked(secondTermPrimary.getDB(dbName)[collName].insert({"num2": 123}));

// Enable a failpoint to hang after transitioning to rollback mode.
const rollbackHangFailPoint =
    configureFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

// Reconnect the isolated node and rollback should start.
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// The connection will be closed during rollback, so retries are needed here.
assert.soonNoExcept(() => {
    rollbackHangFailPoint.wait();
    return true;
}, `failed to wait for fail point ${rollbackHangFailPoint.failPointName}`);

// Enable a failpoint to hang after processing heartbeat reconfig, so we can
// verify that the old primary was successfully removed while rolling back.
const postHbReconfigFailPoint =
    configureFailPoint(rollbackNode, "waitForPostActionCompleteInHbReconfig");

// RollbackTest stopped replication on tie breaker node, need to restart it.
// Otherwise the new config, which contains only the new primary and the tie
// breaker, cannot be majority committed.
restartServerReplication(tieBreakerNode);

// Now, we'll remove the node going through rollback from the replica set.
// The new primary will then propagate the new config to the other nodes via heartbeat.
// When the node in rollback processes a heartbeat from the primary with a new config,
// it will learn it is removed and transition to the RS_REMOVED state.
let newConfig = rollbackTest.getTestFixture().getReplSetConfigFromNode();
const initialMembers = newConfig.members;
newConfig.members = newConfig.members.filter((node) => node.host !== rollbackNode.host);
newConfig.version++;
assert.commandWorked(secondTermPrimary.adminCommand({replSetReconfig: newConfig}));
rollbackTest.getTestFixture().waitForConfigReplication(secondTermPrimary);

assert.soonNoExcept(() => {
    postHbReconfigFailPoint.wait();
    return true;
}, `failed to wait for fail point ${postHbReconfigFailPoint.failPointName}`);

// Verify the rollback node is removed from replica set config.
assert.commandFailedWithCode(rollbackNode.adminCommand({replSetGetStatus: 1}),
                             ErrorCodes.InvalidReplicaSetConfig);

// Now we disable the fail points, allowing the rollback to continue.
postHbReconfigFailPoint.off();
rollbackHangFailPoint.off();

// Add the removed node back the replica set config, so that we can execute the last
// step of the RollbackTest to transition back to steady state.
newConfig = Object.assign({}, rollbackTest.getTestFixture().getReplSetConfigFromNode());
newConfig.members = initialMembers;
newConfig.version++;

assert.commandWorked(secondTermPrimary.adminCommand({replSetReconfig: newConfig}));
rollbackTest.getTestFixture().waitForConfigReplication(secondTermPrimary);

// Verify the removed node is added back and primary sees its state as SECONDARY.
rollbackTest.getTestFixture().waitForState(rollbackNode, ReplSetTest.State.SECONDARY);

// Transition back to steady state.
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.stop();
})();
