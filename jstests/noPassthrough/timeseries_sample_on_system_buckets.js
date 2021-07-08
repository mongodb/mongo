/**
 * Verifies that a direct $sample stage on system.buckets collection works.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

if (!TimeseriesTest.timeseriesCollectionsEnabled(testDB.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

// Prepares a timeseries collection.
assert.commandWorked(
    testDB.createCollection("t", {timeseries: {timeField: "time", metaField: "meta"}}));
assert.commandWorked(
    testDB.t.insert([{time: ISODate(), meta: 1, a: 1}, {time: ISODate(), meta: 1, a: 2}]));

// Verifies that a direct $sample stage on system.buckets collection works.
const kNoOfSamples = 1;
const res = testDB.system.buckets.t.aggregate([{$sample: {size: kNoOfSamples}}]).toArray();
assert.eq(res.length, kNoOfSamples);

MongoRunner.stopMongod(conn);
})();
