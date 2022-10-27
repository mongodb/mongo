/**
 * Tests that buckets which need to be closed due to size (timeseriesBucketMaxSize) are kept open
 * until the number of measurements reaches the threshold (timeseriesBucketMinCount).
 *
 * @tags: [
 *   requires_collstats,
 *   requires_fcv_61,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);
assert.commandWorked(db.dropDatabase());

const coll = db.getCollection(jsTestName());

const timeFieldName = "localTime";
const metaFieldName = "host";
const resetCollection = (() => {
    coll.drop();
    assert.commandWorked(db.createCollection(
        jsTestName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
});

const areTimeseriesScalabilityImprovementsEnabled =
    TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db);

const numMeasurements = 50;
let expectedNumBucketsKeptOpenDueToLargeMeasurements = 0;
const checkBucketSize = (() => {
    const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;

    if (areTimeseriesScalabilityImprovementsEnabled) {
        // Need at least 10 measurements before closing buckets exceeding timeseriesBucketMaxSize.
        assert.eq(numMeasurements / 10, timeseriesStats.bucketCount);

        assert(timeseriesStats.hasOwnProperty("numBucketsKeptOpenDueToLargeMeasurements"));
        expectedNumBucketsKeptOpenDueToLargeMeasurements += numMeasurements / 10;
        assert.eq(expectedNumBucketsKeptOpenDueToLargeMeasurements,
                  timeseriesStats.numBucketsKeptOpenDueToLargeMeasurements);
    } else {
        // After accounting for the control.min and control.max summaries, one measurement of server
        // status exceeds the bucket max size. Which means we'll only have one measurement per
        // bucket.
        assert.eq(numMeasurements, timeseriesStats.bucketCount);

        assert(!timeseriesStats.hasOwnProperty("numBucketsKeptOpenDueToLargeMeasurements"));
    }
});

jsTestLog("Testing single inserts");
resetCollection();

for (let i = 0; i < numMeasurements; i++) {
    const doc = assert.commandWorked(db.runCommand({serverStatus: 1}));
    assert.commandWorked(coll.insert(doc));
}

checkBucketSize();

jsTestLog("Testing batched inserts");
resetCollection();

let batch = [];
for (let i = 0; i < numMeasurements; i++) {
    const doc = assert.commandWorked(db.runCommand({serverStatus: 1}));
    batch.push(doc);
}
assert.commandWorked(coll.insertMany(batch, {ordered: false}));

checkBucketSize();
MongoRunner.stopMongod(conn);
}());
