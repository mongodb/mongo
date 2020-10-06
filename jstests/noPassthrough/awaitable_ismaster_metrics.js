/**
 * Tests the server status metrics of awaitable isMaster.
 * @tags: [requires_replication]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

function runAwaitIsMaster(maxAwaitTimeMS) {
    const res = assert.commandWorked(db.runCommand({isMaster: 1}));
    assert(res.hasOwnProperty("topologyVersion"), res);
    const topologyVersionField = res.topologyVersion;

    assert.commandWorked(db.runCommand(
        {isMaster: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: maxAwaitTimeMS}));
}

function runTest(db, failPoint) {
    const res = assert.commandWorked(db.runCommand({isMaster: 1}));
    assert(res.hasOwnProperty("topologyVersion"), res);

    const topologyVersionField = res.topologyVersion;
    assert(topologyVersionField.hasOwnProperty("processId"), topologyVersionField);
    assert(topologyVersionField.hasOwnProperty("counter"), topologyVersionField);

    // Test that metrics are properly updated when there are isMaster requests that are waiting.
    let awaitIsMasterFailPoint = configureFailPoint(failPoint.conn, failPoint.failPointName);
    let singleAwaitIsMaster =
        startParallelShell(funWithArgs(runAwaitIsMaster, 100), failPoint.conn.port);

    // Ensure the isMaster requests have started waiting before checking the metrics.
    awaitIsMasterFailPoint.wait();
    // awaitingTopologyChanges should increment once.
    let numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChange);
    configureFailPoint(failPoint.conn, failPoint.failPointName, {}, "off");

    // The awaitingTopologyChanges metric should decrement once the waiting isMaster has returned.
    singleAwaitIsMaster();
    numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChange);

    // Refresh the number of times we have entered the failpoint.
    awaitIsMasterFailPoint = configureFailPoint(failPoint.conn, failPoint.failPointName);
    let firstAwaitIsMaster =
        startParallelShell(funWithArgs(runAwaitIsMaster, 100), failPoint.conn.port);
    let secondAwaitIsMaster =
        startParallelShell(funWithArgs(runAwaitIsMaster, 100), failPoint.conn.port);
    assert.commandWorked(db.runCommand({
        waitForFailPoint: failPoint.failPointName,
        // Each failpoint will be entered twice. Once for the 'shouldFail' check and again for the
        // 'pauseWhileSet'.
        timesEntered: awaitIsMasterFailPoint.timesEntered + 4,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(2, numAwaitingTopologyChange);
    configureFailPoint(failPoint.conn, failPoint.failPointName, {}, "off");

    firstAwaitIsMaster();
    secondAwaitIsMaster();
    numAwaitingTopologyChange = db.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChange);
}

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");
// A failpoint signalling that the standalone server has received the IsMaster request and is
// waiting for maxAwaitTimeMS.
let failPoint = configureFailPoint(conn, "hangWaitingForIsMasterResponseOnStandalone");
runTest(conn.getDB("admin"), failPoint);
MongoRunner.stopMongod(conn);

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();
const primary = replTest.getPrimary();
// A failpoint signalling that the server has received the IsMaster request and is waiting for a
// topology change or maxAwaitTimeMS.
failPoint = configureFailPoint(primary, "hangWhileWaitingForHelloResponse");
runTest(primary.getDB("admin"), failPoint);
replTest.stopSet();

const st = new ShardingTest({mongos: 1, shards: [{nodes: 1}], config: 1});
failPoint = configureFailPoint(st.s, "hangWhileWaitingForHelloResponse");
runTest(st.s.getDB("admin"), failPoint);
st.stop();
})();
