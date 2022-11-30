/**
 * Test that calls to read from telemetry store fail when feature flag is turned off and sampling
 * rate > 0.
 */
load('jstests/libs/analyze_plan.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

// This test specifically tests error handling when the feature flag is not on.
if (FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    return;
}

// Set sampling rate to MAX_INT.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 2147483647},
};

const conn = MongoRunner.runMongod(options);
const testdb = conn.getDB('test');
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
    db.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline, with a filter, to read telemetry store fails without feature flag turned on even though
// sampling rate is > 0.
assert.commandFailedWithCode(db.adminCommand({
    aggregate: 1,
    pipeline: [{$telemetry: {}}, {$match: {"key.find.find": {$eq: "###"}}}],
    cursor: {}
}),
                             ErrorCodes.QueryFeatureNotAllowed);

MongoRunner.stopMongod(conn);
}());
