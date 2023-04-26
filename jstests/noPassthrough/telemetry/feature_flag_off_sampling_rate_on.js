/**
 * Test that calls to read from telemetry store fail when feature flag is turned off and sampling
 * rate > 0.
 */
load('jstests/libs/analyze_plan.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

// Set sampling rate to -1.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: -1},
};
const conn = MongoRunner.runMongod(options);
const testdb = conn.getDB('test');

// This test specifically tests error handling when the feature flag is not on.
// TODO SERVER-65800 This test can be deleted when the feature is on by default.
if (!conn || FeatureFlagUtil.isEnabled(testdb, "Telemetry")) {
    jsTestLog(`Skipping test since feature flag is disabled. conn: ${conn}`);
    if (conn) {
        MongoRunner.stopMongod(conn);
    }
    return;
}

var coll = testdb[jsTestName()];
coll.drop();

// Bulk insert documents to reduces roundtrips and make timeout on a slow machine less likely.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= 20; i++) {
    bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
}
assert.commandWorked(bulk.execute());

// Pipeline to read telemetry store should fail without feature flag turned on even though sampling
// rate is > 0.
assert.commandFailedWithCode(
    testdb.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline, with a filter, to read telemetry store fails without feature flag turned on even though
// sampling rate is > 0.
assert.commandFailedWithCode(testdb.adminCommand({
    aggregate: 1,
    pipeline: [{$telemetry: {}}, {$match: {"key.queryShape.find": {$eq: "###"}}}],
    cursor: {}
}),
                             ErrorCodes.QueryFeatureNotAllowed);

MongoRunner.stopMongod(conn);
}());
