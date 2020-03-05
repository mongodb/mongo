/*
 * Tests hedging metrics in the serverStatus output.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

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
 * Verifies that the hedgingMetrics in the server status response is equal to the expected
 * hedgingMetrics.
 */
function checkServerStatusHedgingMetrics(mongosConn, expectedHedgingMetrics) {
    const serverStatus = assert.commandWorked(mongosConn.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);

    assert.eq(expectedHedgingMetrics.numTotalOperations,
              serverStatus.hedgingMetrics.numTotalOperations);
    assert.eq(expectedHedgingMetrics.numTotalHedgedOperations,
              serverStatus.hedgingMetrics.numTotalHedgedOperations);
    assert.eq(expectedHedgingMetrics.numAdvantageouslyHedgedOperations,
              serverStatus.hedgingMetrics.numAdvantageouslyHedgedOperations);
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

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

let expectedHedgingMetrics = {
    numTotalOperations: 0,
    numTotalHedgedOperations: 0,
    numAdvantageouslyHedgedOperations: 0
};

assert.commandWorked(
    testDB.runCommand({query: {find: collName}, $readPreference: {mode: "primary"}}));
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

// force an advantageous hedged read to succeed.
// TODO: RSM may target reads differently in the future sending the first read to secondary. In this
// case this test will fail.
try {
    setCommandDelay(st.rs0.getPrimary(), "find", 1000);
    assert.commandWorked(
        testDB.runCommand({query: {find: collName}, $readPreference: {mode: "nearest"}}));
} finally {
    clearCommandDelay(st.rs0.getPrimary());
}

expectedHedgingMetrics.numTotalOperations += 2;
expectedHedgingMetrics.numTotalHedgedOperations += 1;
expectedHedgingMetrics.numAdvantageouslyHedgedOperations += 1;
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

// force an advantageous hedged read to hang.
// TODO: RSM may target reads differently in the future sending the first read to secondary. In this
// case this test will fail.
try {
    setCommandDelay(st.rs0.getPrimary(), "find", 10);
    setCommandDelay(st.rs0.getSecondaries()[0], "find", 1000);

    assert.commandWorked(
        testDB.runCommand({query: {find: collName}, $readPreference: {mode: "nearest"}}));
} finally {
    clearCommandDelay(st.rs0.nodes[0]);
    clearCommandDelay(st.rs0.nodes[1]);
}

expectedHedgingMetrics.numTotalOperations += 2;
expectedHedgingMetrics.numTotalHedgedOperations += 1;
expectedHedgingMetrics.numAdvantageouslyHedgedOperations += 0;
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);
st.stop();
}());
