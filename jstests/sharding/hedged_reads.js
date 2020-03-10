/*
 * Tests hedging metrics in the serverStatus output.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function setCommandDelay(nodeConn, command, delay) {
    assert.commandWorked(nodeConn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            failInternalCommands: true,
            blockConnection: true,
            blockTimeMS: delay,
            failCommands: [command],
        }
    }));
}

function clearCommandDelay(nodeConn) {
    assert.commandWorked(nodeConn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));
}

/*
 * Returns true if there is operation with the given comment running on the
 * given connection.
 */
function checkForOpWithComment(conn, comment) {
    const ret =
        conn.getDB("admin")
            .aggregate([{$currentOp: {localOps: true}}, {$match: {"command.comment": comment}}])
            .toArray();

    jsTestLog(`Checked currentOp with comment ${comment}: ${tojson(ret)}`);

    if (ret.length == 0) {
        return false;
    }

    if (ret.every(op => op.killPending)) {
        // CurrentOp actually blocks kills from proceeding.
        return false;
    }

    return true;
}

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
const dbName = "hedged_reads";
const collName = "test";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

const kLargeTimeoutMS = 5 * 60 * 1000;
const waitKillOpTimeoutMS = 5 * 1000;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Force the mongos's replica set monitors to always include all the eligible nodes.
const replicaSetMonitorProtocol =
    assert.commandWorked(st.s.adminCommand({getParameter: 1, replicaSetMonitorProtocol: 1}))
        .replicaSetMonitorProtocol;
let serverSelectorFailPoint = configureFailPoint(st.s,
                                                 replicaSetMonitorProtocol === "scanning"
                                                     ? "scanningServerSelectorIgnoreLatencyWindow"
                                                     : "sdamServerSelectorIgnoreLatencyWindow");

jsTest.log(
    "Verify that maxTimeMS expiration of the additional request does not affect the command result");
// The hedged read will have the maxTimeMS set to 10ms, hence need to sleep longer than that.
assert.commandWorked(testDB.runCommand({
    find: collName,
    filter: {$where: "sleep(100); return true;"},
    $readPreference: {mode: "nearest"}
}));

// Force the mongos to send requests to hosts in alphabetical order of host names.
let sendRequestsFailPoint =
    configureFailPoint(st.s, "networkInterfaceSendRequestsToTargetHostsInAlphabeticalOrder");
let sortedNodes = [...st.rs0.nodes].sort((node1, node2) => node1.host.localeCompare(node2.host));

st.rs0.nodes.forEach(function(conn) {
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, logComponentVerbosity: {command: {verbosity: 1}}}));
});

assert.commandWorked(
    st.s.adminCommand({setParameter: 1, logComponentVerbosity: {network: {verbosity: 2}}}));

jsTest.log("Verify the initial request is canceled when the hedged request responds first");
try {
    // Make the initial request block.
    setCommandDelay(sortedNodes[0], "find", kLargeTimeoutMS);

    const comment = "test_kill_initial_request_" + ObjectId();
    assert.commandWorked(testDB.runCommand({
        find: collName,
        filter: {x: {$gte: 0}},
        $readPreference: {mode: "nearest"},
        comment: comment
    }));

    assert.soon(
        function() {
            return !checkForOpWithComment(sortedNodes[0], comment);
        },
        "Timed out waiting for the operation run by the initial request to be killed",
        waitKillOpTimeoutMS);
} finally {
    clearCommandDelay(sortedNodes[0]);
}

jsTest.log("Verify the additional request is canceled when the initial request responds first");
try {
    // Make the additional/hedged request block, set a large maxTimeMSForHedgedReads to prevent
    // the remote host from killing the operation by itself.
    assert.commandWorked(
        st.s.adminCommand({setParameter: 1, maxTimeMSForHedgedReads: kLargeTimeoutMS}));
    setCommandDelay(sortedNodes[1], "find", kLargeTimeoutMS);

    const comment = "test_kill_additional_request_" + ObjectId();
    assert.commandWorked(testDB.runCommand({
        find: collName,
        filter: {x: {$gte: 0}},
        $readPreference: {mode: "nearest"},
        comment: comment
    }));

    assert.soon(
        function() {
            return !checkForOpWithComment(sortedNodes[1], comment);
        },
        "Timed out waiting for the operation run by the additional request to be killed",
        waitKillOpTimeoutMS);
} finally {
    clearCommandDelay(sortedNodes[1]);
}

serverSelectorFailPoint.off();
sendRequestsFailPoint.off();

st.stop();
}());
