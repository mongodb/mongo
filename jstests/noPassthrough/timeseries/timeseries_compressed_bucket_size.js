/**
 * Tests that the bucket size is computed using compressed measurements.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   # Test examines collection stats.
 *   requires_collstats,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesAlwaysUseCompressedBuckets,
 *   requires_fcv_80,
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

const coll = db.getCollection(jsTestName());
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(jsTestName(), {timeseries: {timeField: timeFieldName}}));
const bucketColl = db.getCollection("system.buckets." + jsTestName());

const measurementLength = 1 * 1024;  // 1KB
const numMeasurements = 123;  // The number of measurements before the bucket rolls over due to size
for (let i = 0; i < numMeasurements; i++) {
    // Strings greater than 16 bytes are not compressed unless they are equal to the previous.
    const value = (i % 2 == 0 ? "a" : "b");
    const doc = {
        _id: i,
        [timeFieldName]: ISODate("2024-03-11T00:00:00.000Z"),
        value: value.repeat(measurementLength)
    };
    assert.commandWorked(coll.insert(doc));
}

let buckets = bucketColl.find().toArray();
assert.eq(1, buckets.length);
assert.eq(0, buckets[0].control.min._id);
assert.eq(122, buckets[0].control.max._id);

const timeseriesStats = assert.commandWorked(coll.stats()).timeseries;
jsTestLog("Bucket size: " + timeseriesStats.avgBucketSize);

const timeseriesBucketMaxSize = (() => {
    const res =
        assert.commandWorked(db.adminCommand({getParameter: 1, timeseriesBucketMaxSize: 1}));
    return res.timeseriesBucketMaxSize;
})();

// The bucket size will be slightly over the bucket max size as the change in compression size is
// computed after inserting the measurements into the bucket.
assert.gt(timeseriesStats.avgBucketSize, timeseriesBucketMaxSize);
assert.lte(timeseriesStats.avgBucketSize, timeseriesBucketMaxSize * 1.01);

// Inserting an additional measurement will open a second bucket.
assert.commandWorked(coll.insert({
    _id: 123,
    [timeFieldName]: ISODate("2024-03-11T00:00:00.000Z"),
    value: "c".repeat(measurementLength)
}));
buckets = bucketColl.find().toArray();
assert.eq(2, buckets.length);

MongoRunner.stopMongod(conn);
