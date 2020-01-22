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
}

const st = new ShardingTest({shards: 2});
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

let expectedHedgingMetrics = {numTotalOperations: 0, numTotalHedgedOperations: 0};

assert.commandWorked(
    testDB.runCommand({query: {find: collName}, $readPreference: {mode: "primary"}}));
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

assert.commandWorked(
    testDB.runCommand({query: {find: collName}, $readPreference: {mode: "nearest", hedge: {}}}));
checkServerStatusHedgingMetrics(testDB, expectedHedgingMetrics);

st.stop();
}());
