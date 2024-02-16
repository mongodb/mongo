/**
 * Tests that buckets which need to be closed due to size (timeseriesBucketMaxSize) are kept open
 * until the number of measurements reaches the threshold (timeseriesBucketMinCount).
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

const timeFieldName = "localTime";
const metaFieldName = "host";
const resetCollection = (() => {
    coll.drop();
    assert.commandWorked(db.createCollection(
        jsTestName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
});

const numMeasurements = 50;
let expectedNumBucketsKeptOpenDueToLargeMeasurements = 0;
const checkBucketSize = (() => {
    const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;

    // Need at least 10 measurements before closing buckets exceeding timeseriesBucketMaxSize.
    assert.eq(numMeasurements / 10, timeseriesStats.bucketCount);

    assert(timeseriesStats.hasOwnProperty("numBucketsKeptOpenDueToLargeMeasurements"));
    assert.eq(numMeasurements / 10, timeseriesStats.numBucketsKeptOpenDueToLargeMeasurements);
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