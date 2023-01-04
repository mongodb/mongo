/**
 * Test that the $telemetry.redactionFieldNames parameter correctly sets the redaction stratgey for
 * telemetry store keys.
 */
load("jstests/libs/feature_flag_util.js");    // For FeatureFlagUtil.
load("jstests/aggregation/extras/utils.js");  // For assertAdminDBErrCodeAndErrMsgContains.

(function() {
"use strict";

if (!FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    return;
}

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 2147483647},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

coll.aggregate([{$sort: {bar: -1}}, {$limit: 2}, {$match: {foo: {$lte: 2}}}]);
// Default is no redaction.
let telStore = assert.commandWorked(
    testDB.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}}));
assert.eq(telStore.cursor.firstBatch[0]["key"]["pipeline"],
          [{"$sort": {"bar": "###"}}, {"$limit": "###"}, {"$match": {"foo": {"$lte": "###"}}}]);

// Turning on redaction should redact field names on all entries, even previously cached ones.
telStore = assert.commandWorked(testDB.adminCommand(
    {aggregate: 1, pipeline: [{$telemetry: {redactFieldNames: true}}], cursor: {}}));
telStore.cursor.firstBatch.forEach(element => {
    // Find the non-telemetry query and check its key to assert it matches requested redaction
    // strategy.
    if (!telStore.cursor.firstBatch[0]["key"]["pipeline"][0]["$telemetry"]) {
        assert.eq(telStore.cursor.firstBatch[0]["key"]["pipeline"], [
            {"$sort": {"/N4rLtula/QI": "###"}},
            {"$limit": "###"},
            {"$match": {"LCa0a2j/xo/5": {"TmACc7vp8cv6": "###"}}}
        ]);
    }
});

// Turning redaction back off should preserve field names on all entries, even previously cached
// ones.
telStore = assert.commandWorked(
    testDB.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}}));
telStore.cursor.firstBatch.forEach(element => {
    // Find the non-telemetry query and check its key to assert it matches requested redaction
    // strategy.
    if (!telStore.cursor.firstBatch[0]["key"]["pipeline"][0]["$telemetry"]) {
        assert.eq(
            telStore.cursor.firstBatch[0]["key"]["pipeline"],
            [{"$sort": {"bar": "###"}}, {"$limit": "###"}, {"$match": {"foo": {"$lte": "###"}}}]);
    }
});

// Explicitly set redactFieldNames to false.
telStore = assert.commandWorked(testDB.adminCommand(
    {aggregate: 1, pipeline: [{$telemetry: {redactFieldNames: false}}], cursor: {}}));
telStore.cursor.firstBatch.forEach(element => {
    // Find the non-telemetry query and check its key to assert it matches requested redaction
    // strategy.
    if (!telStore.cursor.firstBatch[0]["key"]["pipeline"][0]["$telemetry"]) {
        assert.eq(
            telStore.cursor.firstBatch[0]["key"]["pipeline"],
            [{"$sort": {"bar": "###"}}, {"$limit": "###"}, {"$match": {"foo": {"$lte": "###"}}}]);
    }
});

// Wrong parameter name throws error.
let pipeline = [{$telemetry: {redactFields: true}}];
assertAdminDBErrCodeAndErrMsgContains(
    coll,
    pipeline,
    ErrorCodes.FailedToParse,
    "$telemetry parameters object may only contain 'redactFieldNames' option. Found: redactFields");

// Wrong parameter type throws error.
pipeline = [{$telemetry: {redactFieldNames: 1}}];
assertAdminDBErrCodeAndErrMsgContains(
    coll,
    pipeline,
    ErrorCodes.FailedToParse,
    "$telemetry redactFieldNames parameter must be boolean. Found type: double");

// Parameter object with unrecognized key throws error.
pipeline = [{$telemetry: {redactFieldNames: true, redactionStrategy: "on"}}];
assertAdminDBErrCodeAndErrMsgContains(
    coll,
    pipeline,
    ErrorCodes.FailedToParse,
    "$telemetry parameters object may only contain one field, 'redactFieldNames'. Found: { redactFieldNames: true, redactionStrategy: \"on\" }");

MongoRunner.stopMongod(conn);
}());
