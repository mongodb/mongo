/**
 * Test that calls to read from telemetry store fail when sampling rate is not greater than 0 even
 * if feature flag is on.
 * @tags: [featureFlagTelemetry]
 */
load('jstests/libs/analyze_plan.js');

(function() {
"use strict";

let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 0},
};

const conn = MongoRunner.runMongod(options);
const testdb = conn.getDB('test');
var coll = testdb[jsTestName()];
coll.drop();
for (var i = 0; i < 20; i++) {
    coll.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
}

coll.aggregate([{$match: {foo: 1}}], {cursor: {batchSize: 2}});

// Reading telemetry store with a sampling rate of 0 should return 0 documents.
let telStore = testdb.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}});
assert.eq(telStore.cursor.firstBatch.length, 0);

// Reading telemetry store should work now with a sampling rate of greater than 0.
assert.commandWorked(testdb.adminCommand(
    {setParameter: 1, internalQueryConfigureTelemetrySamplingRate: 2147483647}));
coll.aggregate([{$match: {foo: 1}}], {cursor: {batchSize: 2}});
telStore = assert.commandWorked(
    testdb.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}}));
assert.eq(telStore.cursor.firstBatch.length, 1);

MongoRunner.stopMongod(conn);
}());
