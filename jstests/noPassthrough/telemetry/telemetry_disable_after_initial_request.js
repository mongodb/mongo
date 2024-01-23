/**
 * Tests that telemetry metrics aren't collected if the feature is enabled initially but is disabled
 * before the lifetime of the request is complete.
 * @tags: [featureFlagTelemetry]
 */
load("jstests/libs/telemetry_utils.js");  // For getTelemetryFindCmd.

// Test that no telemetry entry is written when (1) dispatching an initial find query, (2)
// disabling telemetry, then (3) completing the command. Below, we run variations of this test
// with combinations of different strategies to disable telemetry and to end the command.
function testStatsAreNotCollectedWhenDisabledBeforeCommandCompletion(
    {conn, coll, disableTelemetryFn, endCommandFn, enableTelemetryFn}) {
    // Issue a find commannd with a batchSize of 1 so that the query is not exhausted.
    const cursor = coll.find({foo: 1}).batchSize(1);
    // Must run .next() to make sure the initial request is executed now.
    cursor.next();

    // Disable telemetry, then end the command,which triggers the path to writeTelemetry.
    disableTelemetryFn();
    endCommandFn(cursor);

    // Must re-enable telemetry in order to check via $telemetry that nothing was recorded.
    enableTelemetryFn();
    const res = getTelemetryFindCmd(conn, {collName: coll, redactIdentifiers: false});
    assert.eq(res.length, 0, res);
}

// Turn on the collecting of telemetry metrics.
let options = {setParameter: {internalQueryConfigureTelemetrySamplingRate: -1}};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 100;
for (let i = 0; i < numDocs / 2; ++i) {
    bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
    bulk.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
}
assert.commandWorked(bulk.execute());

function setTelemetryCacheSize(size) {
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryConfigureTelemetryCacheSize: size}));
}

// Tests the scenario of disabling telemetry by setting internalQueryConfigureTelemetryCacheSize to
// 0 and ending the command by running it to completion.
testStatsAreNotCollectedWhenDisabledBeforeCommandCompletion({
    conn: testDB,
    coll,
    disableTelemetryFn: () => setTelemetryCacheSize("0MB"),
    endCommandFn: (cursor) => cursor.itcount(),
    enableTelemetryFn: () => setTelemetryCacheSize("10MB")
});

// Tests the scenario of disabling telemetry by setting internalQueryConfigureTelemetryCacheSize to
// 0 and ending the command by killing the cursor.
testStatsAreNotCollectedWhenDisabledBeforeCommandCompletion({
    conn: testDB,
    coll,
    disableTelemetryFn: () => setTelemetryCacheSize("0MB"),
    endCommandFn: (cursor) => assert.commandWorked(
        testDB.runCommand({killCursors: coll.getName(), cursors: [cursor.getId()]})),
    enableTelemetryFn: () => setTelemetryCacheSize("10MB")
});

MongoRunner.stopMongod(conn);