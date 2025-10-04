/**
 * Tests that the space usage calculation for new fields in time-series inserts accounts for the
 * control.min and control.max fields.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Test examines collection stats.
 *   requires_collstats,
 *   # Large measurement handling changed in binVersion 6.1.
 *   requires_fcv_61,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.getCollection(jsTestName());

const timeFieldName = "time";
const resetCollection = () => {
    coll.drop();
    assert.commandWorked(db.createCollection(jsTestName(), {timeseries: {timeField: timeFieldName}}));
};

const timeseriesBucketMaxSize = (() => {
    const res = assert.commandWorked(db.adminCommand({getParameter: 1, timeseriesBucketMaxSize: 1}));
    return res.timeseriesBucketMaxSize;
})();

const checkAverageBucketSize = () => {
    const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;

    jsTestLog("Average bucket size: " + timeseriesStats.avgBucketSize);
    assert.lte(timeseriesStats.avgBucketSize, timeseriesBucketMaxSize);

    const firstBucket = getTimeseriesCollForRawOps(db, coll).find().rawData().sort({"control.min._id": 1})[0];
    assert.eq(0, firstBucket.control.min._id);
    assert.eq(9, firstBucket.control.max._id);
};

// Each measurement inserted will consume roughly 1/12th of the bucket max size. In theory, we'll
// only be able to fit ten measurements per bucket. The first measurement will also create the
// control.min and control.max summaries, which will account for two measurements worth of data.
// The other measurements will not modify the control.min and control.max fields to the same degree
// as they're going to insert the same-length values. The remaining ~4% of the bucket size is left
// for other internal fields that need to be written out.
const measurementValueLength = Math.floor(timeseriesBucketMaxSize * 0.08);

const numMeasurements = 100;

jsTestLog("Testing single inserts");
resetCollection();

for (let i = 0; i < numMeasurements; i++) {
    // Strings greater than 16 bytes are not compressed unless they are equal to the previous.
    const value = i % 2 == 0 ? "a" : "b";
    // Increment the timestamp to test ordering of documents. If the same timestamp
    // were given to all measurements, there would be no guarantee on ordering.
    let timestamp = new Date(ISODate("2024-01-01T01:00:00Z").getTime() + i * 1000);
    const doc = {_id: i, [timeFieldName]: timestamp, value: value.repeat(measurementValueLength)};
    assert.commandWorked(coll.insert(doc));
}
checkAverageBucketSize();

jsTestLog("Testing batched inserts");
resetCollection();

let batch = [];
for (let i = 0; i < numMeasurements; i++) {
    // Strings greater than 16 bytes are not compressed unless they are equal to the previous.
    const value = i % 2 == 0 ? "a" : "b";
    // Increment the timestamp to test ordering of documents. If the same timestamp
    // were given to all measurements, there would be no guarantee on ordering.
    let timestamp = new Date(ISODate("2024-01-01T01:00:00Z").getTime() + i * 1000);
    const doc = {_id: i, [timeFieldName]: timestamp, value: value.repeat(measurementValueLength)};
    batch.push(doc);
}
assert.commandWorked(coll.insertMany(batch), {ordered: false});
checkAverageBucketSize();

MongoRunner.stopMongod(conn);
