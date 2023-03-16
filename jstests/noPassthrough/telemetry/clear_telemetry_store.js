/**
 * Test that the telemetry store can be cleared when the cache size is reset to 0.
 * @tags: [featureFlagTelemetry]
 */
load("jstests/libs/telemetry_utils.js");  // For verifyMetrics.

(function() {
"use strict";

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {
        internalQueryConfigureTelemetrySamplingRate: 2147483647,
        internalQueryConfigureTelemetryCacheSize: "10MB"
    },
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

let query = {};
for (var j = 0; j < 10; ++j) {
    query["foo.field.xyz." + j] = 1;
    query["bar.field.xyz." + j] = 2;
    query["baz.field.xyz." + j] = 3;
    coll.aggregate([{$match: query}]).itcount();
}

// Confirm number of entries in the store and that none have been evicted.
let telemetryResults = testDB.getSiblingDB("admin").aggregate([{$telemetry: {}}]).toArray();
assert.eq(telemetryResults.length, 10, telemetryResults);
assert.eq(testDB.serverStatus().metrics.telemetry.numEvicted, 0);

// Command to clear the cache.
assert.commandWorked(
    testDB.adminCommand({setParameter: 1, internalQueryConfigureTelemetryCacheSize: "0MB"}));

// 10 regular queries plus the $telemetry query, means 11 entries evicted when the cache is cleared.
assert.eq(testDB.serverStatus().metrics.telemetry.numEvicted, 11);

// Calling $telemetry should fail when the telemetry store size is 0 bytes.
assert.throwsWithCode(() => testDB.getSiblingDB("admin").aggregate([{$telemetry: {}}]), 6579000);
MongoRunner.stopMongod(conn);
}());
