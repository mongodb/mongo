/**
 * Tests that buckets which are kept open until the number of measurements reaches the threshold
 * (timeseriesBucketMinCount) are closed when the bucket is close to the max BSON size limit.
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
const bucketColl = db.getCollection("system.buckets." + jsTestName());

const timeFieldName = "time";
const resetCollection = (() => {
    coll.drop();
    assert.commandWorked(
        db.createCollection(jsTestName(), {timeseries: {timeField: timeFieldName}}));
});

const areTimeseriesScalabilityImprovementsEnabled =
    TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db);

const numMeasurements = 4;
let expectedNumBucketsClosedDueToSize = 0;
let expectedNumBucketsKeptOpenDueToLargeMeasurements = 0;
const checkBucketSize = (() => {
    const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;

    if (areTimeseriesScalabilityImprovementsEnabled) {
        // Buckets with large measurements are kept open after exceeding timeseriesBucketMaxSize
        // until they have 10 measurements. However, if the bucket size were to exceed 12MB, it gets
        // closed regardless.
        const bucketDocs = bucketColl.find().sort({'control.min._id': 1}).toArray();
        assert.eq(2, bucketDocs.length, bucketDocs);

        // First bucket should be full with three documents.
        assert.eq(0, bucketDocs[0].control.min._id);
        assert.eq(2, bucketDocs[0].control.max._id);

        // Second bucket should contain the remaining document.
        assert.eq(numMeasurements - 1, bucketDocs[1].control.min._id);
        assert.eq(numMeasurements - 1, bucketDocs[1].control.max._id);

        assert.eq(++expectedNumBucketsClosedDueToSize, timeseriesStats.numBucketsClosedDueToSize);
        assert.eq(++expectedNumBucketsKeptOpenDueToLargeMeasurements,
                  timeseriesStats.numBucketsKeptOpenDueToLargeMeasurements);
    } else {
        // Only one measurement per bucket without time-series scalability improvements.
        const bucketDocs = bucketColl.find().sort({'control.min._id': 1}).toArray();
        assert.eq(numMeasurements, bucketDocs.length, bucketDocs);

        assert(!timeseriesStats.hasOwnProperty("numBucketsKeptOpenDueToLargeMeasurements"));
    }
});

const measurementValueLength = 2 * 1024 * 1024;

jsTestLog("Testing single inserts");
resetCollection();

for (let i = 0; i < numMeasurements; i++) {
    const doc = {_id: i, [timeFieldName]: ISODate(), value: "a".repeat(measurementValueLength)};
    assert.commandWorked(coll.insert(doc));
}
checkBucketSize();

jsTestLog("Testing batched inserts");
resetCollection();

let batch = [];
for (let i = 0; i < numMeasurements; i++) {
    const doc = {_id: i, [timeFieldName]: ISODate(), value: "a".repeat(measurementValueLength)};
    batch.push(doc);
}
assert.commandWorked(coll.insertMany(batch, {ordered: false}));

checkBucketSize();
MongoRunner.stopMongod(conn);
}());
