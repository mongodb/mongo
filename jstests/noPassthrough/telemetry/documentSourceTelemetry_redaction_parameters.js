/**
 * Test the $telemetry redaction properties.
 * @tags: [featureFlagTelemetry]
 */

load("jstests/aggregation/extras/utils.js");  // For assertAdminDBErrCodeAndErrMsgContains.
load("jstests/libs/telemetry_utils.js");

(function() {
"use strict";

// Assert the expected telemetry key with no redaction.
function assertUnredactedTelemetryKey(telemetryKey) {
    assert.eq(telemetryKey.queryShape.filter, {"foo": {"$lte": "?number"}});
    assert.eq(telemetryKey.queryShape.sort, {"bar": -1});
    assert.eq(telemetryKey.queryShape.limit, "?number");
}

function runTest(conn) {
    const testDB = conn.getDB('test');
    var coll = testDB[jsTestName()];
    coll.drop();

    coll.insert({foo: 1});
    coll.find({foo: {$lte: 2}}).sort({bar: -1}).limit(2).toArray();
    // Default is no redaction.
    assertUnredactedTelemetryKey(getTelemetry(conn)[0].key);

    // Turning on redaction should redact field names on all entries, even previously cached ones.
    const telemetryKey = getTelemetryRedacted(conn)[0]["key"];
    assert.eq(telemetryKey.queryShape.filter,
              {"fNWkKfogMv6MJ77LpBcuPrO7Nq+R+7TqtD+Lgu3Umc4=": {"$lte": "?number"}});
    assert.eq(telemetryKey.queryShape.sort, {"CDDQIXZmDehLKmQcRxtdOQjMqoNqfI2nGt2r4CgJ52o=": -1});
    assert.eq(telemetryKey.queryShape.limit, "?number");

    // Turning redaction back off should preserve field names on all entries, even previously cached
    // ones.
    assertUnredactedTelemetryKey(getTelemetry(conn)[0]["key"]);

    // Explicitly set redactIdentifiers to false.
    assertUnredactedTelemetryKey(getTelemetryRedacted(conn, false)[0]["key"]);

    // Wrong parameter name throws error.
    let pipeline = [{$telemetry: {redactFields: true}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$telemetry parameters object may only contain 'redactIdentifiers' or 'redactionKey' options. Found: redactFields");

    // Wrong parameter type throws error.
    pipeline = [{$telemetry: {redactIdentifiers: 1}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$telemetry redactIdentifiers parameter must be boolean. Found type: double");

    pipeline = [{$telemetry: {redactionKey: 1}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$telemetry redactionKey parameter must be bindata of length 32 or greater. Found type: double");

    // Parameter object with unrecognized key throws error.
    pipeline = [{$telemetry: {redactIdentifiers: true, redactionStrategy: "on"}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$telemetry parameters object may only contain 'redactIdentifiers' or 'redactionKey' options. Found: redactionStrategy");
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryConfigureTelemetrySamplingRate: -1,
        featureFlagTelemetry: true,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryConfigureTelemetrySamplingRate: -1,
            featureFlagTelemetry: true,
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
        }
    },
});
runTest(st.s);
st.stop();
}());
