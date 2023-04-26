/**
 * Tests that doing a reconfig that changes the SplitHorizon will cause the server to disconnect
 * from clients with waiting hello/isMaster requests.
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

const dbName = "awaitable_command_horizon_change";
const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const secondary = replTest.getSecondary();
const secondaryDB = secondary.getDB(dbName);

function runAwaitableCmdBeforeHorizonChange(cmd, topologyVersionField) {
    let res = assert.throws(() => db.runCommand({
        [cmd]: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert(isNetworkError(res));

    // We should automatically reconnect after the failed command.
    assert.commandWorked(db.adminCommand({ping: 1}));
}

function runAwaitableCmd(cmd, topologyVersionField) {
    const result = assert.commandWorked(db.runCommand({
        [cmd]: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, result.topologyVersion.counter);
}

let idx = 0;
// runTest takes in the hello command or its aliases, isMaster and ismaster.
function runTest(cmd) {
    const primaryFirstResponse = assert.commandWorked(primaryDB.runCommand({[cmd]: 1}));
    const primaryTopologyVersion = primaryFirstResponse.topologyVersion;

    const secondaryFirstResponse = assert.commandWorked(secondaryDB.runCommand({[cmd]: 1}));
    const secondaryTopologyVersion = secondaryFirstResponse.topologyVersion;

    // A failpoint signalling that the server has received the hello/isMaster request and is waiting
    // for a topology change.
    let primaryFailPoint = configureFailPoint(primary, "waitForHelloResponse");
    let secondaryFailPoint = configureFailPoint(secondary, "waitForHelloResponse");
    // Send an awaitable hello/isMaster request. This will block until there is a topology change.
    const awaitCmdHorizonChangeOnPrimary = startParallelShell(
        funWithArgs(runAwaitableCmdBeforeHorizonChange, cmd, primaryTopologyVersion), primary.port);
    const awaitCmdHorizonChangeOnSecondary = startParallelShell(
        funWithArgs(runAwaitableCmdBeforeHorizonChange, cmd, secondaryTopologyVersion),
        secondary.port);
    primaryFailPoint.wait();
    secondaryFailPoint.wait();

    // Each node has one hello/isMaster request waiting on a topology change.
    let numAwaitingTopologyChangeOnPrimary =
        primaryDB.serverStatus().connections.awaitingTopologyChanges;
    let numAwaitingTopologyChangeOnSecondary =
        secondaryDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChangeOnPrimary);
    assert.eq(1, numAwaitingTopologyChangeOnSecondary);

    // Doing a reconfig that changes the horizon should respond to all waiting hello/isMaster
    // requests with an error.
    let rsConfig = primary.getDB("local").system.replset.findOne();
    rsConfig.members.forEach(function(member) {
        member.horizons = {specialHorizon: 'horizon.com:100' + idx};
        idx++;
    });
    rsConfig.version++;

    jsTest.log('Calling replSetReconfig with config: ' + tojson(rsConfig));
    assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}));
    awaitCmdHorizonChangeOnPrimary();
    awaitCmdHorizonChangeOnSecondary();

    // All hello/isMaster requests should have been responded to after the reconfig.
    numAwaitingTopologyChangeOnPrimary =
        primaryDB.serverStatus().connections.awaitingTopologyChanges;
    numAwaitingTopologyChangeOnSecondary =
        secondaryDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChangeOnPrimary);
    assert.eq(0, numAwaitingTopologyChangeOnSecondary);

    const primaryRespAfterHorizonChange = assert.commandWorked(primaryDB.runCommand({[cmd]: 1}));
    const secondaryRespAfterHorizonChange =
        assert.commandWorked(secondaryDB.runCommand({[cmd]: 1}));
    const primaryTopVersionAfterHorizonChange = primaryRespAfterHorizonChange.topologyVersion;
    const secondaryTopVersionAfterHorizonChange = secondaryRespAfterHorizonChange.topologyVersion;

    // Doing a reconfig that doesn't change the horizon should increment the topologyVersion and
    // reply to waiting hello/isMaster requests with a successful response.
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
    primaryFailPoint = configureFailPoint(primary, "waitForHelloResponse");
    secondaryFailPoint = configureFailPoint(primary, "waitForHelloResponse");
    // Send an awaitable hello/isMaster request. This will block until maxAwaitTimeMS has elapsed or
    // a topology change happens.
    let primaryAwaitCmdBeforeAddingTags = startParallelShell(
        funWithArgs(runAwaitableCmd, cmd, primaryTopVersionAfterHorizonChange), primary.port);
    let secondaryAwaitCmdBeforeAddingTags = startParallelShell(
        funWithArgs(runAwaitableCmd, cmd, secondaryTopVersionAfterHorizonChange), secondary.port);
    primaryFailPoint.wait();
    secondaryFailPoint.wait();

    jsTest.log('Calling replSetReconfig with config: ' + tojson(rsConfig));
    assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}));
    primaryAwaitCmdBeforeAddingTags();
    secondaryAwaitCmdBeforeAddingTags();
}

runTest("hello");
runTest("isMaster");
runTest("ismaster");
replTest.stopSet();
})();
