/**
 * Test the $queryStats hmac properties.
 * @tags: [featureFlagQueryStats]
 */

load("jstests/aggregation/extras/utils.js");  // For assertAdminDBErrCodeAndErrMsgContains.
load("jstests/libs/telemetry_utils.js");

(function() {
"use strict";

// Assert the expected telemetry key with no hmac.
function assertTelemetryKeyWithoutHmac(telemetryKey) {
    assert.eq(telemetryKey.filter, {"foo": {"$lte": "?number"}});
    assert.eq(telemetryKey.sort, {"bar": -1});
    assert.eq(telemetryKey.limit, "?number");
}

function runTest(conn) {
    const testDB = conn.getDB('test');
    var coll = testDB[jsTestName()];
    coll.drop();

    coll.insert({foo: 1});
    coll.find({foo: {$lte: 2}}).sort({bar: -1}).limit(2).toArray();
    // Default is no hmac.
    assertTelemetryKeyWithoutHmac(getQueryStatsFindCmd(conn)[0].key.queryShape);

    // Turning on hmac should apply hmac to all field names on all entries, even previously cached
    // ones.
    const telemetryKey = getQueryStatsFindCmd(conn, /*applyHmacToIdentifiers*/ true)[0]["key"];
    assert.eq(telemetryKey.queryShape.filter,
              {"fNWkKfogMv6MJ77LpBcuPrO7Nq+R+7TqtD+Lgu3Umc4=": {"$lte": "?number"}});
    assert.eq(telemetryKey.queryShape.sort, {"CDDQIXZmDehLKmQcRxtdOQjMqoNqfI2nGt2r4CgJ52o=": -1});
    assert.eq(telemetryKey.queryShape.limit, "?number");

    // Turning hmac back off should preserve field names on all entries, even previously cached
    // ones.
    const telemetry = getTelemetry(conn)[1]["key"];
    assertTelemetryKeyWithoutHmac(telemetry.queryShape);

    // Explicitly set applyHmacToIdentifiers to false.
    assertTelemetryKeyWithoutHmac(
        getQueryStatsFindCmd(conn, /*applyHmacToIdentifiers*/ false)[0]["key"].queryShape);

    // Wrong parameter name throws error.
    let pipeline = [{$queryStats: {redactFields: true}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats parameters object may only contain 'applyHmacToIdentifiers' or 'hmacKey' options. Found: redactFields");

    // Wrong parameter type throws error.
    pipeline = [{$queryStats: {applyHmacToIdentifiers: 1}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats applyHmacToIdentifiers parameter must be boolean. Found type: double");

    pipeline = [{$queryStats: {hmacKey: 1}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats hmacKey parameter must be bindata of length 32 or greater. Found type: double");

    // Parameter object with unrecognized key throws error.
    pipeline = [{$queryStats: {applyHmacToIdentifiers: true, hmacStrategy: "on"}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats parameters object may only contain 'applyHmacToIdentifiers' or 'hmacKey' options. Found: hmacStrategy");
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsSamplingRate: -1,
        featureFlagQueryStats: true,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

// TODO SERVER-77325 reenable these tests
// const st = new ShardingTest({
//     mongos: 1,
//     shards: 1,
//     config: 1,
//     rs: {nodes: 1},
//     mongosOptions: {
//         setParameter: {
//             internalQueryStatsSamplingRate: -1,
//             featureFlagQueryStats: true,
//             'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
//         }
//     },
// });
// runTest(st.s);
// st.stop();
}());
