/**
 * Verifies that a direct $sample stage on system.buckets collection works.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

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
