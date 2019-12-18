/**
 * Tests the maxAwaitTimeMS and topologyVersion parameters of the isMaster command.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");

// Test isMaster paramaters on a single node replica set.
const replSetName = "awaitable_ismaster_test";
const replTest = new ReplSetTest({name: replSetName, nodes: 1});
replTest.startSet();
replTest.initiate();

const dbName = "awaitable_ismaster_test";
const node = replTest.getPrimary();
const db = node.getDB(dbName);

// Check isMaster response contains a topologyVersion even if maxAwaitTimeMS and topologyVersion are
// not included in the request.
const res = assert.commandWorked(db.runCommand({isMaster: 1}));
assert(res.hasOwnProperty("topologyVersion"), tojson(res));

const topologyVersionField = res.topologyVersion;
assert(topologyVersionField.hasOwnProperty("processId"), tojson(topologyVersionField));
assert(topologyVersionField.hasOwnProperty("counter"), tojson(topologyVersionField));

// Check that isMaster succeeds when passed a valid toplogyVersion and maxAwaitTimeMS. In this case,
// use the topologyVersion from the previous isMaster response. The topologyVersion field is
// expected to be of the form {processId: <ObjectId>, counter: <long>}.
assert.commandWorked(
    db.runCommand({isMaster: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 0}));

// Ensure isMaster waits for at least maxAwaitTimeMS before returning.
const now = new Date();
assert.commandWorked(
    db.runCommand({isMaster: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 2000}));
const isMasterDuration = new Date() - now;

// Allow for some clock imprecision between the server and the jstest.
assert.gte(isMasterDuration,
           1000,
           `isMaster should have taken at least 1000ms, but completed in ${isMasterDuration}ms`);

// Check that passing a topologyVersion not of type object fails.
assert.commandFailedWithCode(
    db.runCommand({isMaster: 1, topologyVersion: "topology_version_string", maxAwaitTimeMS: 0}),
    10065);

// Check that a topologyVersion with an invalid processId and valid counter fails.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: {processId: "pid1", counter: topologyVersionField.counter},
    maxAwaitTimeMS: 0
}),
                             ErrorCodes.TypeMismatch);

// Check that a topologyVersion with a valid processId and invalid counter fails.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: {processId: topologyVersionField.processId, counter: 0},
    maxAwaitTimeMS: 0
}),
                             ErrorCodes.TypeMismatch);

// Check that a topologyVersion with a valid processId but missing counter fails.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: {processId: topologyVersionField.processId},
    maxAwaitTimeMS: 0
}),
                             40414);

// Check that a topologyVersion with a missing processId and valid counter fails.
assert.commandFailedWithCode(
    db.runCommand(
        {isMaster: 1, topologyVersion: {counter: topologyVersionField.counter}, maxAwaitTimeMS: 0}),
    40414);

// Check that a topologyVersion with a valid processId and negative counter fails.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: {processId: topologyVersionField.processId, counter: NumberLong("-1")},
    maxAwaitTimeMS: 0
}),
                             31372);

// Check that isMaster fails if there is an extra field in its topologyVersion.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: {
        processId: topologyVersionField.processId,
        counter: topologyVersionField.counter,
        randomField: "I should cause an error"
    },
    maxAwaitTimeMS: 0
}),
                             40415);

// A client following the awaitable isMaster protocol must include topologyVersion in their request
// if and only if they include maxAwaitTimeMS.
// Check that isMaster fails if there is a topologyVersion but no maxAwaitTimeMS field.
assert.commandFailedWithCode(db.runCommand({isMaster: 1, topologyVersion: topologyVersionField}),
                             31368);

// Check that isMaster fails if there is a maxAwaitTimeMS field but no topologyVersion.
assert.commandFailedWithCode(db.runCommand({isMaster: 1, maxAwaitTimeMS: 0}), 31368);

// Check that isMaster fails if there is a valid topologyVersion but invalid maxAwaitTimeMS type.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: topologyVersionField,
    maxAwaitTimeMS: "stringMaxAwaitTimeMS"
}),
                             ErrorCodes.TypeMismatch);

// Check that isMaster fails if there is a valid topologyVersion but negative maxAwaitTimeMS.
assert.commandFailedWithCode(db.runCommand({
    isMaster: 1,
    topologyVersion: topologyVersionField,
    maxAwaitTimeMS: -1,
}),
                             31373);

function runAwaitableIsMaster(topologyVersionField) {
    const resAfterDisablingWrites = assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(topologyVersionField.counter + 1, resAfterDisablingWrites.topologyVersion.counter);
    // Validate that an isMaster response returns once writes have been disabled on the primary
    // even though the node has yet to transition to secondary.
    assert.eq(false, resAfterDisablingWrites.ismaster);
    assert.eq(false, resAfterDisablingWrites.secondary);

    // The TopologyVersion from resAfterDisablingWrites should now be stale since the old primary
    // has completed its transition to secondary. This isMaster request should respond immediately.
    const resAfterStepdownComplete = assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: resAfterDisablingWrites.topologyVersion,
        maxAwaitTimeMS: 99999999,
    }));
    assert.eq(resAfterDisablingWrites.topologyVersion.counter + 1,
              resAfterStepdownComplete.topologyVersion.counter);
    assert.eq(false, resAfterStepdownComplete.ismaster);
    assert.eq(true, resAfterStepdownComplete.secondary);
}

assert.commandWorked(db.adminCommand({setParameter: 1, logComponentVerbosity: {verbosity: 1}}));
// Send an awaitable isMaster request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let awaitIsMaster =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionField), node.port);
checkLog.contains(node, "Waiting for an isMaster response");
// Call stepdown to increment the server TopologyVersion and respond to the waiting isMaster
// request. We expect stepDown to increment the TopologyVersion twice - once for when the writes are
// disabled and once again for when the primary completes its transition to secondary.
assert.commandWorked(db.adminCommand({replSetStepDown: 60, force: true}));
awaitIsMaster();

replTest.stopSet();
})();
