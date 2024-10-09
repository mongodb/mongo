/**
 * Tests the server status metrics of awaitable hello/isMaster.
 * @tags: [requires_replication]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runAwaitCmd(cmd, maxAwaitTimeMS) {
    const res = assert.commandWorked(db.runCommand({[cmd]: 1}));
    assert(res.hasOwnProperty("topologyVersion"), res);
    const topologyVersionField = res.topologyVersion;

    assert.commandWorked(db.runCommand(
        {[cmd]: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: maxAwaitTimeMS}));
}

function runTest(db, cmd, failPoint, useGRPCStats) {
    const res = assert.commandWorked(db.runCommand({[cmd]: 1}));
    assert(res.hasOwnProperty("topologyVersion"), res);

    const topologyVersionField = res.topologyVersion;
    assert(topologyVersionField.hasOwnProperty("processId"), topologyVersionField);
    assert(topologyVersionField.hasOwnProperty("counter"), topologyVersionField);

    const connectionStats = () => {
        return useGRPCStats ? db.serverStatus().gRPC.streams : db.serverStatus().connections;
    };

    // Test that metrics are properly updated when there are command requests that are waiting.
    let awaitCmdFailPoint = configureFailPoint(failPoint.conn, failPoint.failPointName);
    let singleAwaitCmd =
        startParallelShell(funWithArgs(runAwaitCmd, cmd, 100), failPoint.conn.port);

    // Ensure the command requests have started waiting before checking the metrics.
    awaitCmdFailPoint.wait();
    // awaitingTopologyChanges should increment once.
    let numAwaitingTopologyChange = connectionStats().awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChange);
    configureFailPoint(failPoint.conn, failPoint.failPointName, {}, "off");

    // The awaitingTopologyChanges metric should decrement once the waiting command has returned.
    singleAwaitCmd();
    numAwaitingTopologyChange = connectionStats().awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChange);

    // Refresh the number of times we have entered the failpoint.
    awaitCmdFailPoint = configureFailPoint(failPoint.conn, failPoint.failPointName);
    let firstAwaitCmd = startParallelShell(funWithArgs(runAwaitCmd, cmd, 100), failPoint.conn.port);
    let secondAwaitCmd =
        startParallelShell(funWithArgs(runAwaitCmd, cmd, 100), failPoint.conn.port);
    assert.commandWorked(db.runCommand({
        waitForFailPoint: failPoint.failPointName,
        // Each failpoint will be entered twice. Once for the 'shouldFail' check and again for the
        // 'pauseWhileSet'.
        timesEntered: awaitCmdFailPoint.timesEntered + 4,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    numAwaitingTopologyChange = connectionStats().awaitingTopologyChanges;
    assert.eq(2, numAwaitingTopologyChange);
    configureFailPoint(failPoint.conn, failPoint.failPointName, {}, "off");

    firstAwaitCmd();
    secondAwaitCmd();
    numAwaitingTopologyChange = connectionStats().awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChange);
}

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");
// A failpoint signalling that the standalone server has received the command request and is
// waiting for maxAwaitTimeMS.
let failPoint = configureFailPoint(conn, "hangWaitingForHelloResponseOnStandalone");
runTest(conn.getDB("admin"), "hello", failPoint, conn.isGRPC());
runTest(conn.getDB("admin"), "isMaster", failPoint, conn.isGRPC());
runTest(conn.getDB("admin"), "ismaster", failPoint, conn.isGRPC());
MongoRunner.stopMongod(conn);

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();
const primary = replTest.getPrimary();
// A failpoint signalling that the server has received the command request and is waiting for a
// topology change or maxAwaitTimeMS.
failPoint = configureFailPoint(primary, "hangWhileWaitingForHelloResponse");
runTest(primary.getDB("admin"), "hello", failPoint, primary.isGRPC());
runTest(primary.getDB("admin"), "isMaster", failPoint, primary.isGRPC());
runTest(primary.getDB("admin"), "ismaster", failPoint, primary.isGRPC());
replTest.stopSet();

const st = new ShardingTest({mongos: 1, shards: [{nodes: 1}], config: 1});
failPoint = configureFailPoint(st.s, "hangWhileWaitingForHelloResponseMongos");
runTest(st.s.getDB("admin"), "hello", failPoint, st.s.isGRPC());
runTest(st.s.getDB("admin"), "isMaster", failPoint, st.s.isGRPC());
runTest(st.s.getDB("admin"), "ismaster", failPoint, st.s.isGRPC());
st.stop();
