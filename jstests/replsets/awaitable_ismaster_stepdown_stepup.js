/**
 * Tests the fields returned by hello responses as a node goes through a step down and step up.
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

// Test hello paramaters on a single node replica set.
const replSetName = "awaitable_hello_stepup";
const replTest = new ReplSetTest({name: replSetName, nodes: 1});
replTest.startSet();
replTest.initiate();

const dbName = "awaitable_hello_test";
const node = replTest.getPrimary();
const db = node.getDB(dbName);

// Check hello response contains a topologyVersion even if maxAwaitTimeMS and topologyVersion are
// not included in the request.
const res = assert.commandWorked(db.runCommand({hello: 1}));
assert(res.hasOwnProperty("topologyVersion"), tojson(res));

const topologyVersionField = res.topologyVersion;
assert(topologyVersionField.hasOwnProperty("processId"), tojson(topologyVersionField));
assert(topologyVersionField.hasOwnProperty("counter"), tojson(topologyVersionField));

function runAwaitableHelloBeforeStepDown(topologyVersionField) {
    const resAfterDisablingWrites = assert.commandWorked(db.runCommand({
        hello: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, resAfterDisablingWrites.topologyVersion.counter);
    // Validate that an hello response returns once writes have been disabled on the primary
    // even though the node has yet to transition to secondary.
    assert.eq(false, resAfterDisablingWrites.isWritablePrimary, resAfterDisablingWrites);
    assert.eq(false, resAfterDisablingWrites.secondary, resAfterDisablingWrites);
    assert.hasFields(resAfterDisablingWrites, ["primary"]);

    // The TopologyVersion from resAfterDisablingWrites should now be stale since the old primary
    // has completed its transition to secondary. This hello request should respond immediately.
    const resAfterStepdownComplete = assert.commandWorked(db.runCommand({
        hello: 1,
        topologyVersion: resAfterDisablingWrites.topologyVersion,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(resAfterDisablingWrites.topologyVersion.counter + 1,
              resAfterStepdownComplete.topologyVersion.counter,
              resAfterStepdownComplete);
    assert.eq(false, resAfterStepdownComplete.isWritablePrimary, resAfterStepdownComplete);
    assert.eq(true, resAfterStepdownComplete.secondary, resAfterStepdownComplete);
    assert(!resAfterStepdownComplete.hasOwnProperty("primary"), resAfterStepdownComplete);
}

function runAwaitableHelloBeforeStepUp(topologyVersionField) {
    const resAfterEnteringDrainMode = assert.commandWorked(db.runCommand({
        hello: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, resAfterEnteringDrainMode.topologyVersion.counter);
    // Validate that the hello response returns once the primary enters drain mode. At this
    // point, we expect the 'primary' field to exist but 'isWritablePrimary' will still be false.
    assert.eq(false, resAfterEnteringDrainMode.isWritablePrimary, resAfterEnteringDrainMode);
    assert.eq(true, resAfterEnteringDrainMode.secondary, resAfterEnteringDrainMode);
    assert.hasFields(resAfterEnteringDrainMode, ["primary"]);

    // The TopologyVersion from resAfterEnteringDrainMode should now be stale since we expect
    // the primary to increase the config term and increment the counter once again.
    const resAfterReconfigOnStepUp = assert.commandWorked(db.runCommand({
        hello: 1,
        topologyVersion: resAfterEnteringDrainMode.topologyVersion,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(resAfterEnteringDrainMode.topologyVersion.counter + 1,
              resAfterReconfigOnStepUp.topologyVersion.counter,
              resAfterReconfigOnStepUp);
    assert.eq(false, resAfterReconfigOnStepUp.isWritablePrimary, resAfterReconfigOnStepUp);
    assert.eq(true, resAfterReconfigOnStepUp.secondary, resAfterReconfigOnStepUp);
    assert.hasFields(resAfterReconfigOnStepUp, ["primary"]);
}

function runAwaitableHelloAfterStepUp(topologyVersionField) {
    // The TopologyVersion from resAfterReconfigOnStepUp should now be stale since we expect
    // the primary to exit drain mode and increment the counter once again.
    const resAfterExitingDrainMode = assert.commandWorked(db.runCommand({
        hello: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, resAfterExitingDrainMode.topologyVersion.counter);
    assert.eq(true, resAfterExitingDrainMode.isWritablePrimary, resAfterExitingDrainMode);
    assert.eq(false, resAfterExitingDrainMode.secondary, resAfterExitingDrainMode);
    assert.hasFields(resAfterExitingDrainMode, ["primary"]);
}

// A failpoint signalling that the server has received the hello request and is waiting for a
// topology change.
let failPoint = configureFailPoint(node, "waitForIsMasterResponse");
// Send an awaitable hello request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let awaitHelloBeforeStepDown = startParallelShell(
    funWithArgs(runAwaitableHelloBeforeStepDown, topologyVersionField), node.port);
failPoint.wait();

// Call stepdown to increment the server TopologyVersion and respond to the waiting hello
// request. We expect stepDown to increment the TopologyVersion twice - once for when the writes are
// disabled and once again for when the primary completes its transition to secondary.
assert.commandWorked(db.adminCommand({replSetStepDown: 60, force: true}));
awaitHelloBeforeStepDown();

let response = assert.commandWorked(node.getDB(dbName).runCommand({hello: 1}));
assert(response.hasOwnProperty("topologyVersion"), tojson(res));
const topologyVersionAfterStepDown = response.topologyVersion;

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
failPoint = configureFailPoint(node, "waitForIsMasterResponse");
const hangFailPoint = configureFailPoint(node, "hangAfterReconfigOnDrainComplete");
// Send an awaitable hello request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let awaitHelloBeforeStepUp = startParallelShell(
    funWithArgs(runAwaitableHelloBeforeStepUp, topologyVersionAfterStepDown), node.port);
failPoint.wait();

// Unfreezing the old primary will cause the node to step up in a single node replica set.
assert.commandWorked(node.adminCommand({replSetFreeze: 0}));

// Wait until stepup thread hangs after the reconfig.
hangFailPoint.wait();
awaitHelloBeforeStepUp();

response = assert.commandWorked(node.getDB(dbName).runCommand({hello: 1}));
assert(response.hasOwnProperty("topologyVersion"), tojson(res));
const topologyVersionAfterStepUp = response.topologyVersion;

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
failPoint = configureFailPoint(node, "waitForIsMasterResponse");
// Send an awaitable hello request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let awaitHelloAfterStepUp = startParallelShell(
    funWithArgs(runAwaitableHelloAfterStepUp, topologyVersionAfterStepUp), node.port);
failPoint.wait();
// Let the stepup thread to continue.
hangFailPoint.off();
awaitHelloAfterStepUp();

replTest.stopSet();
})();
