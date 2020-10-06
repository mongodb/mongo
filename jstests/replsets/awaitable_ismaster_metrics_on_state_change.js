/**
 * Tests that the server status metrics correctly reflect the number of waiting isMaster requests
 * before and after a state change.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

// Test isMaster paramaters on a single node replica set.
const replTest = new ReplSetTest({name: "awaitable_ismaster_metrics", nodes: 1});
replTest.startSet();
replTest.initiate();

const dbName = "awaitable_ismaster_metrics";
const node = replTest.getPrimary();
const db = node.getDB(dbName);

const res = assert.commandWorked(db.runCommand({isMaster: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);

const topologyVersionField = res.topologyVersion;
assert(topologyVersionField.hasOwnProperty("processId"), topologyVersionField);
assert(topologyVersionField.hasOwnProperty("counter"), topologyVersionField);

function runAwaitableIsMaster(topologyVersionField) {
    const res = assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, res.topologyVersion.counter);
}

// A failpoint signalling that the server has received the IsMaster request and is waiting for a
// topology change.
let failPoint = configureFailPoint(node, "waitForHelloResponse");
// Send an awaitable isMaster request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let firstIsMasterBeforeStepDown =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionField), node.port);
failPoint.wait();
// awaitingTopologyChanges should increment once.
let numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
assert.eq(1, numAwaitingTopologyChange);

// Reconfigure failpoint to refresh the number of times entered.
failPoint = configureFailPoint(node, "waitForHelloResponse");
let secondIsMasterBeforeStepdown =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionField), node.port);
failPoint.wait();
numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
assert.eq(2, numAwaitingTopologyChange);

// Call stepdown to increment the server TopologyVersion and respond to the waiting isMaster
// requests.
assert.commandWorked(db.adminCommand({replSetStepDown: 60, force: true}));
firstIsMasterBeforeStepDown();
secondIsMasterBeforeStepdown();
// All isMaster requests should have been responded to.
numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
assert.eq(0, numAwaitingTopologyChange);

replTest.stopSet();
})();
