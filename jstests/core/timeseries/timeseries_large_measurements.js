/**
 * Tests that the space usage calculation for new fields in time-series inserts accounts for the
 * control.min and control.max fields.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_collstats,
 *   requires_fcv_61,
 * ]
 */
(function() {
"use strict";

const coll = db.getCollection(jsTestName());

const timeFieldName = "time";
const resetCollection = (() => {
    coll.drop();
    assert.commandWorked(
        db.createCollection(jsTestName(), {timeseries: {timeField: timeFieldName}}));
});

const timeseriesBucketMaxSize = (() => {
    const res =
        assert.commandWorked(db.adminCommand({getParameter: 1, timeseriesBucketMaxSize: 1}));
    return res.timeseriesBucketMaxSize;
})();

const checkAverageBucketSize = (() => {
    const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
    const averageBucketSize = timeseriesStats.numBytesUncompressed / timeseriesStats.bucketCount;

    jsTestLog("Average bucket size: " + averageBucketSize);
    assert.lte(averageBucketSize, timeseriesBucketMaxSize);
});

// Each measurement inserted will consume roughly 1/5th of the bucket max size. In theory, we'll
// only be able to fit three measurements per bucket. The first measurement will also create the
// control.min and control.max summaries, which will account for two measurements worth of data.
// The second and third measurements will not modify the control.min and control.max fields to the
// same degree as they're going to insert the same-length values. The remaining ~5% of the bucket
// size is left for other internal fields that need to be written out.
const measurementValueLength = Math.floor(timeseriesBucketMaxSize * 0.19);

const numMeasurements = 100;

jsTestLog("Testing single inserts");
resetCollection();

for (let i = 0; i < numMeasurements; i++) {
    const doc = {[timeFieldName]: ISODate(), value: "a".repeat(measurementValueLength)};
    assert.commandWorked(coll.insert(doc));
}
checkAverageBucketSize();

jsTestLog("Testing batched inserts");
resetCollection();

let batch = [];
for (let i = 0; i < numMeasurements; i++) {
    const doc = {[timeFieldName]: ISODate(), value: "a".repeat(measurementValueLength)};
    batch.push(doc);
}
assert.commandWorked(coll.insertMany(batch));
checkAverageBucketSize();
}());
