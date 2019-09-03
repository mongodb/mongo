/**
 * This test documents the behavior that rolling back immediately after upgrading
 * enableMajorityReadConcern to true can fassert. If this happens, the user can restart the server
 * with enableMajorityReadConcern=false to complete the rollback, then upgrade again to
 * enableMajorityReadConcern=true.
 * Rollback after restarting with enableMajorityReadConcern=true succeeds if the common point is at
 * least the stable timestamp, i.e. we do not attempt to roll back operations that were included in
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");
load("jstests/libs/write_concern_util.js");

TestData.rollbackShutdowns = true;
const name = "rollback_after_enabling_majority_reads";
const dbName = "test";
const collName = "coll";

jsTest.log("Set up a Rollback Test with enableMajorityReadConcern=false");
const replTest = new ReplSetTest(
    {name, nodes: 3, useBridge: true, nodeOptions: {enableMajorityReadConcern: "false"}});
replTest.startSet();
let config = replTest.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
replTest.initiate(config);
let rollbackTest = new RollbackTest(name, replTest);

jsTest.log("Ensure the stable timestamp is ahead of the common point on the rollback node.");
const rollbackNode = rollbackTest.transitionToRollbackOperations();
const operationTime = assert
                          .commandWorked(rollbackNode.getDB(dbName).runCommand(
                              {insert: collName, documents: [{_id: "rollback op"}]}))
                          .operationTime;

// Do a clean shutdown to ensure the recovery timestamp is at operationTime.
jsTest.log("Restart the rollback node with enableMajorityReadConcern=true");
rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "true"});
const replSetGetStatusResponse =
    assert.commandWorked(rollbackNode.adminCommand({replSetGetStatus: 1}));
assert.eq(replSetGetStatusResponse.lastStableRecoveryTimestamp,
          operationTime,
          tojson(replSetGetStatusResponse));

// The rollback crashes because the common point is before the stable timestamp.
jsTest.log("Attempt to roll back. This will fassert.");
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
assert.soon(() => {
    return rawMongoProgramOutput().indexOf("Fatal Assertion 51121") !== -1;
});

jsTest.log(
    "Restart the rollback node with enableMajorityReadConcern=false. Now the rollback can succeed.");
const allowedExitCode = 14;
rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "false"}, allowedExitCode);

// Fix counts for "local.startup_log", since they are corrupted by this rollback.
// transitionToSteadyStateOperations() checks collection counts.
assert.commandWorked(rollbackNode.getDB("local").runCommand({validate: "startup_log"}));
rollbackTest.transitionToSteadyStateOperations();

assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert(
    {_id: "steady state op"}, {writeConcern: {w: "majority"}}));

assert.eq(0, rollbackNode.getDB(dbName)[collName].find({_id: "rollback op"}).itcount());
assert.eq(1, rollbackNode.getDB(dbName)[collName].find({_id: "steady state op"}).itcount());

jsTest.log("Restart the rollback node with enableMajorityReadConcern=true.");
rollbackTest.restartNode(0, 15, {enableMajorityReadConcern: "true"});

// Make sure node 0 is the primary.
let node = replTest.nodes[0];
jsTestLog("Waiting for node " + node.host + " to become primary.");
replTest.awaitNodesAgreeOnPrimary();
assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
replTest.waitForState(node, ReplSetTest.State.PRIMARY);
assert.eq(replTest.getPrimary(), node, node.host + " was not primary after step up.");

// Restart replication on the tiebreaker node before constructing a new RollbackTest.
restartServerReplication(replTest.nodes[2]);

// Create a new RollbackTest fixture to execute the final rollback. This will guarantee that the
// final rollback occurs on the current primary, which should be node 0.
jsTestLog("Creating a new RollbackTest fixture to execute a final rollback.");
rollbackTest = new RollbackTest(name, replTest);

jsTest.log("Rollback should succeed since the common point is at least the stable timestamp.");
rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

rollbackTest.stop();
}());