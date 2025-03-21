/**
 * Tests that buckets which are kept open until the number of measurements reaches the threshold
 * (timeseriesBucketMinCount) are closed when the bucket is close to the max BSON size limit.
 *
 * @tags: [
 *   requires_collstats,
 *   requires_fcv_61,
 * ]
 */
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

const numMeasurements = 4;
const checkBucketSize = (() => {
    const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;

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

    // TODO(SERVER-102744): re-enable check
    // assert.eq(1, timeseriesStats.numBucketsClosedDueToSize);
    assert.eq(1, timeseriesStats.numBucketsKeptOpenDueToLargeMeasurements);
});

const measurementValueLength = 2 * 1024 * 1024;

jsTestLog("Testing single inserts");
resetCollection();

for (let i = 0; i < numMeasurements; i++) {
    // Strings greater than 16 bytes are not compressed unless they are equal to the previous.
    const value = (i % 2 == 0 ? "a" : "b");
    // Increment the timestamp to test ordering of documents. If the same timestamp
    // were given to all measurements, there would be no guarantee on ordering.
    let timestamp = new Date(ISODate("2024-01-01T01:00:00Z").getTime() + i * 1000);
    const doc = {_id: i, [timeFieldName]: timestamp, value: value.repeat(measurementValueLength)};
    assert.commandWorked(coll.insert(doc));
}
checkBucketSize();

jsTestLog("Testing batched inserts");
resetCollection();

let batch = [];
for (let i = 0; i < numMeasurements; i++) {
    // Strings greater than 16 bytes are not compressed unless they are equal to the previous.
    const value = (i % 2 == 0 ? "a" : "b");
    // Increment the timestamp to test ordering of documents. If the same timestamp
    // were given to all measurements, there would be no guarantee on ordering.
    let timestamp = new Date(ISODate("2024-01-01T01:00:00Z").getTime() + i * 1000);
    const doc = {_id: i, [timeFieldName]: timestamp, value: value.repeat(measurementValueLength)};
    batch.push(doc);
}
assert.commandWorked(coll.insertMany(batch, {ordered: false}));

checkBucketSize();
MongoRunner.stopMongod(conn);
