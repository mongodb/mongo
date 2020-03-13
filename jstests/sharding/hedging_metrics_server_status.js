/*
 * Tests hedging metrics in the serverStatus output.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

/*
 * Verifies that the server status response has the hegingMetrics fields that we expect.
 */
function verifyServerStatusFields(serverStatusResponse) {
    assert(serverStatusResponse.hasOwnProperty("hedgingMetrics"),
           "Expected the serverStatus response to have a 'hedgingMetrics' field\n" +
               tojson(serverStatusResponse));
    assert(
        serverStatusResponse.hedgingMetrics.hasOwnProperty("numTotalOperations"),
        "The 'hedgingMetrics' field in serverStatus did not have the 'numTotalOperations' field\n" +
            tojson(serverStatusResponse.hedgingMetrics));
    assert(
        serverStatusResponse.hedgingMetrics.hasOwnProperty("numTotalHedgedOperations"),
        "The 'hedgingMetrics' field in serverStatus did not have the 'numTotalHedgedOperations' field\n" +
            tojson(serverStatusResponse.hedgingMetrics));
    assert(
        serverStatusResponse.hedgingMetrics.hasOwnProperty("numAdvantageouslyHedgedOperations"),
        "The 'hedgingMetrics' field in serverStatus did not have the 'numAdvantageouslyHedgedOperations' field\n" +
            tojson(serverStatusResponse.hedgingMetrics));
}

/*
 * Verifies that eventually the hedgingMetrics in the server status response is equal to
 * the expected hedgingMetrics.
 */
function checkServerStatusHedgingMetrics(mongosConn, expectedHedgingMetrics) {
    assert.soon(
        () => {
            const serverStatus = assert.commandWorked(mongosConn.adminCommand({serverStatus: 1}));
            verifyServerStatusFields(serverStatus);
            return bsonWoCompare(serverStatus.hedgingMetrics, expectedHedgingMetrics) === 0;
        },
        `expect the hedgingMetrics to eventually be equal to ${tojson(expectedHedgingMetrics)}`,
        serverStatusCheckTimeoutMS);
}

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

const st = new ShardingTest({shards: 1, rs: {nodes: 2}, mongosOptions: {verbose: 2}});
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);
const serverStatusCheckTimeoutMS = 5000;

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
// Force the mongos to connect to eligible hosts in alphabetical order of host names.
let targetHostsInAlphabeticalOrderFailPoint =
    configureFailPoint(st.s, "networkInterfaceConnectTargetHostsInAlphabeticalOrder");
let sortedNodes = [...st.rs0.nodes].sort((node1, node2) => node1.host.localeCompare(node2.host));

let expectedHedgingMetrics = {
    numTotalOperations: 0,
    numTotalHedgedOperations: 0,
    numAdvantageouslyHedgedOperations: 0
};

jsTestLog("Run a command with hedging disabled, and verify the metrics does not change");
assert.commandWorked(testDB.runCommand(
    {count: collName, query: {x: {gte: 0}}, $readPreference: {mode: "primaryPreferred"}}));
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

jsTestLog("Run commands with hedging enabled, and verify the metrics are as expected");

// Make the command slower on the first target host, and verify there is an advantageous
// hedged read.
try {
    setCommandDelay(sortedNodes[0], "count", 1000);
    assert.commandWorked(testDB.runCommand(
        {count: collName, query: {x: {gte: 0}}, $readPreference: {mode: "nearest"}}));
} finally {
    clearCommandDelay(sortedNodes[0]);
}

expectedHedgingMetrics.numTotalOperations += 1;
expectedHedgingMetrics.numTotalHedgedOperations += 1;
expectedHedgingMetrics.numAdvantageouslyHedgedOperations += 1;
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

// Make the command slower on the second target host, and verify there is no advantageous
// hedged read.
try {
    setCommandDelay(sortedNodes[0], "count", 10);
    setCommandDelay(sortedNodes[1], "count", 1000);

    assert.commandWorked(testDB.runCommand(
        {count: collName, query: {x: {gte: 0}}, $readPreference: {mode: "nearest"}}));
} finally {
    clearCommandDelay(sortedNodes[0]);
    clearCommandDelay(sortedNodes[1]);
}

expectedHedgingMetrics.numTotalOperations += 1;
expectedHedgingMetrics.numTotalHedgedOperations += 1;
expectedHedgingMetrics.numAdvantageouslyHedgedOperations += 0;
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

serverSelectorFailPoint.off();
targetHostsInAlphabeticalOrderFailPoint.off();
st.stop();
}());
