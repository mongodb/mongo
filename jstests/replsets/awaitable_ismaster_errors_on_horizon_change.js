/**
 * Tests that doing a reconfig that changes the SplitHorizon will cause the server to disconnect
 * from clients with waiting isMaster requests.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

// Never retry on network error, because this test needs to detect the network error.
TestData.skipRetryOnNetworkError = true;

const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
replTest.startSet();
replTest.initiate();

const dbName = "awaitable_ismaster_horizon_change";
const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const secondary = replTest.getSecondary();
const secondaryDB = secondary.getDB(dbName);

function runAwaitableIsMasterBeforeHorizonChange(topologyVersionField) {
    let res = assert.throws(() => db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert(isNetworkError(res));

    // We should automatically reconnect after the failed command.
    assert.commandWorked(db.adminCommand({ping: 1}));
}

function runAwaitableIsMaster(topologyVersionField) {
    const result = assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, result.topologyVersion.counter);
}

const primaryFirstResponse = assert.commandWorked(primaryDB.runCommand({isMaster: 1}));
const primaryTopologyVersion = primaryFirstResponse.topologyVersion;

const secondaryFirstResponse = assert.commandWorked(secondaryDB.runCommand({isMaster: 1}));
const secondaryTopologyVersion = secondaryFirstResponse.topologyVersion;

// A failpoint signalling that the server has received the isMaster request and is waiting for a
// topology change.
let primaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
let secondaryFailPoint = configureFailPoint(secondary, "waitForIsMasterResponse");
// Send an awaitable isMaster request. This will block until there is a topology change.
const awaitIsMasterHorizonChangeOnPrimary = startParallelShell(
    funWithArgs(runAwaitableIsMasterBeforeHorizonChange, primaryTopologyVersion), primary.port);
const awaitIsMasterHorizonChangeOnSecondary = startParallelShell(
    funWithArgs(runAwaitableIsMasterBeforeHorizonChange, secondaryTopologyVersion), secondary.port);
primaryFailPoint.wait();
secondaryFailPoint.wait();

// Each node has one isMaster request waiting on a topology change.
let numAwaitingTopologyChangeOnPrimary =
    primaryDB.serverStatus().connections.awaitingTopologyChanges;
let numAwaitingTopologyChangeOnSecondary =
    secondaryDB.serverStatus().connections.awaitingTopologyChanges;
assert.eq(1, numAwaitingTopologyChangeOnPrimary);
assert.eq(1, numAwaitingTopologyChangeOnSecondary);

// Doing a reconfig that changes the horizon should respond to all waiting isMasters with an error.
let rsConfig = primary.getDB("local").system.replset.findOne();
let idx = 0;
rsConfig.members.forEach(function(member) {
    member.horizons = {specialHorizon: 'horizon.com:100' + idx};
    idx++;
});
rsConfig.version++;

jsTest.log('Calling replSetReconfig with config: ' + tojson(rsConfig));
assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}));
awaitIsMasterHorizonChangeOnPrimary();
awaitIsMasterHorizonChangeOnSecondary();

// All isMaster requests should have been responded to after the reconfig.
numAwaitingTopologyChangeOnPrimary = primaryDB.serverStatus().connections.awaitingTopologyChanges;
numAwaitingTopologyChangeOnSecondary =
    secondaryDB.serverStatus().connections.awaitingTopologyChanges;
assert.eq(0, numAwaitingTopologyChangeOnPrimary);
assert.eq(0, numAwaitingTopologyChangeOnSecondary);

const primaryRespAfterHorizonChange = assert.commandWorked(primaryDB.runCommand({isMaster: 1}));
const secondaryRespAfterHorizonChange = assert.commandWorked(secondaryDB.runCommand({isMaster: 1}));
const primaryTopVersionAfterHorizonChange = primaryRespAfterHorizonChange.topologyVersion;
const secondaryTopVersionAfterHorizonChange = secondaryRespAfterHorizonChange.topologyVersion;

// Doing a reconfig that doesn't change the horizon should increment the topologyVersion and reply
// to waiting isMasters with a successful response.
rsConfig = primary.getDB("local").system.replset.findOne();
rsConfig.members.forEach(function(member) {
    if (member.host == primary.host) {
        member.tags = {dc: 'ny'};
    } else {
        member.tags = {dc: 'sf'};
    }
});
rsConfig.version++;

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
primaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
secondaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
// Send an awaitable isMaster request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let primaryAwaitIsMasterBeforeAddingTags = startParallelShell(
    funWithArgs(runAwaitableIsMaster, primaryTopVersionAfterHorizonChange), primary.port);
let secondaryAaitIsMasterBeforeAddingTags = startParallelShell(
    funWithArgs(runAwaitableIsMaster, secondaryTopVersionAfterHorizonChange), secondary.port);
primaryFailPoint.wait();
secondaryFailPoint.wait();

jsTest.log('Calling replSetReconfig with config: ' + tojson(rsConfig));
assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}));
primaryAwaitIsMasterBeforeAddingTags();
secondaryAaitIsMasterBeforeAddingTags();

replTest.stopSet();
})();