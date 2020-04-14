/**
 * Tests the streamable isMaster protocol against nodes with invalid replica set configs.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

// Never retry on network error, because this test needs to detect the network error.
TestData.skipRetryOnNetworkError = true;

const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
// Start the replica set but don't initiate yet.
replTest.startSet();

const dbName = "awaitable_ismaster_horizon_change";
const node0 = replTest.nodes[0];
const node1 = replTest.nodes[1];
const dbNode0 = node0.getDB(dbName);
const dbNode1 = node1.getDB(dbName);

let responseNode0 = assert.commandWorked(dbNode0.runCommand({isMaster: 1}));
let responseNode1 = assert.commandWorked(dbNode1.runCommand({isMaster: 1}));
let topologyVersionNode0 = responseNode0.topologyVersion;
let topologyVersionNode1 = responseNode1.topologyVersion;

function runAwaitableIsMaster(topologyVersionField) {
    const result = assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, result.topologyVersion.counter, result);
}

// Waiting isMasters should error when a node rejoins a replica set.
function runAwaitableIsMasterOnRejoiningSet(topologyVersionField) {
    const result = assert.throws(() => db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert(isNetworkError(result));

    // We should automatically reconnect after the failed command.
    assert.commandWorked(db.adminCommand({ping: 1}));
}

// A failpoint signalling that the servers have received the isMaster request and are waiting for a
// topology change.
let node0FailPoint = configureFailPoint(node0, "waitForIsMasterResponse");
let node1FailPoint = configureFailPoint(node1, "waitForIsMasterResponse");
// Send an awaitable isMaster request. This will block until there is a topology change.
const firstAwaitInitiateOnNode0 =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionNode0), node0.port);
const firstAwaitInitiateOnNode1 =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionNode1), node1.port);
node0FailPoint.wait();
node1FailPoint.wait();

// Each node has one isMaster request waiting on a topology change.
let numAwaitingTopologyChangeOnNode0 = dbNode0.serverStatus().connections.awaitingTopologyChanges;
let numAwaitingTopologyChangeOnNode1 = dbNode1.serverStatus().connections.awaitingTopologyChanges;
assert.eq(1, numAwaitingTopologyChangeOnNode0);
assert.eq(1, numAwaitingTopologyChangeOnNode1);

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
node0FailPoint = configureFailPoint(node0, "waitForIsMasterResponse");
node1FailPoint = configureFailPoint(node1, "waitForIsMasterResponse");
const secondAwaitInitiateOnNode0 =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionNode0), node0.port);
const secondAwaitInitiateOnNode1 =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionNode1), node1.port);
node0FailPoint.wait();
node1FailPoint.wait();

// Each node has two isMaster requests waiting on a topology change.
numAwaitingTopologyChangeOnNode0 = dbNode0.serverStatus().connections.awaitingTopologyChanges;
numAwaitingTopologyChangeOnNode1 = dbNode1.serverStatus().connections.awaitingTopologyChanges;
assert.eq(2, numAwaitingTopologyChangeOnNode0);
assert.eq(2, numAwaitingTopologyChangeOnNode1);

// Doing a replSetInitiate should respond to all waiting isMasters.
replTest.initiate();
firstAwaitInitiateOnNode0();
firstAwaitInitiateOnNode1();
secondAwaitInitiateOnNode0();
secondAwaitInitiateOnNode1();

numAwaitingTopologyChangeOnNode0 = dbNode0.serverStatus().connections.awaitingTopologyChanges;
numAwaitingTopologyChangeOnNode1 = dbNode1.serverStatus().connections.awaitingTopologyChanges;
assert.eq(0, numAwaitingTopologyChangeOnNode0);
assert.eq(0, numAwaitingTopologyChangeOnNode1);

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let primaryDB = primary.getDB('admin');
let secondaryDB = secondary.getDB('admin');
const primaryRespAfterInitiate = assert.commandWorked(primaryDB.runCommand({isMaster: 1}));
let primaryTopologyVersion = primaryRespAfterInitiate.topologyVersion;

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
let primaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
const awaitPrimaryIsMasterBeforeNodeRemoval =
    startParallelShell(funWithArgs(runAwaitableIsMaster, primaryTopologyVersion), primary.port);
primaryFailPoint.wait();

// The primary has one isMaster request waiting on a topology change.
let numAwaitingTopologyChangeOnPrimary =
    primaryDB.serverStatus().connections.awaitingTopologyChanges;
assert.eq(1, numAwaitingTopologyChangeOnPrimary);

// Doing a reconfig that removes the secondary should respond to all waiting isMasters.
let config = replTest.getReplSetConfig();
config.members.splice(1, 1);
config.version = replTest.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primaryDB.runCommand({replSetReconfig: config}));
awaitPrimaryIsMasterBeforeNodeRemoval();

// Wait for secondary to realize it is removed.
assert.soonNoExcept(
    () => assert.commandFailedWithCode(secondaryDB.adminCommand({replSetGetStatus: 1}),
                                       ErrorCodes.InvalidReplicaSetConfig));

const primaryRespAfterRemoval = assert.commandWorked(primaryDB.runCommand({isMaster: 1}));
const secondaryRespAfterRemoval = assert.commandWorked(secondaryDB.runCommand({isMaster: 1}));
primaryTopologyVersion = primaryRespAfterRemoval.topologyVersion;
let secondaryTopologyVersion = secondaryRespAfterRemoval.topologyVersion;
assert.eq(false, secondaryRespAfterRemoval.ismaster, secondaryRespAfterRemoval);
assert.eq(false, secondaryRespAfterRemoval.secondary, secondaryRespAfterRemoval);
assert.eq("Does not have a valid replica set config",
          secondaryRespAfterRemoval.info,
          secondaryRespAfterRemoval);

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
primaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
let secondaryFailPoint = configureFailPoint(secondary, "waitForIsMasterResponse");
const awaitPrimaryIsMasterBeforeReadding =
    startParallelShell(funWithArgs(runAwaitableIsMaster, primaryTopologyVersion), primary.port);
const firstAwaitSecondaryIsMasterBeforeRejoining = startParallelShell(
    funWithArgs(runAwaitableIsMasterOnRejoiningSet, secondaryTopologyVersion), secondary.port);
primaryFailPoint.wait();
secondaryFailPoint.wait();

numAwaitingTopologyChangeOnPrimary = primaryDB.serverStatus().connections.awaitingTopologyChanges;
let numAwaitingTopologyChangeOnSecondary =
    secondaryDB.serverStatus().connections.awaitingTopologyChanges;
assert.eq(1, numAwaitingTopologyChangeOnPrimary);
assert.eq(1, numAwaitingTopologyChangeOnSecondary);

// Send a second isMaster to the removed secondary.
secondaryFailPoint = configureFailPoint(secondary, "waitForIsMasterResponse");
const secondAwaitSecondaryIsMasterBeforeRejoining = startParallelShell(
    funWithArgs(runAwaitableIsMasterOnRejoiningSet, secondaryTopologyVersion), secondary.port);
secondaryFailPoint.wait();

numAwaitingTopologyChangeOnSecondary =
    secondaryDB.serverStatus().connections.awaitingTopologyChanges;
assert.eq(2, numAwaitingTopologyChangeOnSecondary);

// Have the secondary rejoin the set. This should respond to waiting isMasters on both nodes.
config = replTest.getReplSetConfig();
config.version = replTest.getReplSetConfigFromNode().version + 1;
assert.commandWorked(primaryDB.runCommand({replSetReconfig: config}));
awaitPrimaryIsMasterBeforeReadding();
firstAwaitSecondaryIsMasterBeforeRejoining();
secondAwaitSecondaryIsMasterBeforeRejoining();

numAwaitingTopologyChangeOnPrimary = primaryDB.serverStatus().connections.awaitingTopologyChanges;
numAwaitingTopologyChangeOnSecondary =
    secondaryDB.serverStatus().connections.awaitingTopologyChanges;
assert.eq(0, numAwaitingTopologyChangeOnPrimary);
assert.eq(0, numAwaitingTopologyChangeOnSecondary);

replTest.stopSet();
})();