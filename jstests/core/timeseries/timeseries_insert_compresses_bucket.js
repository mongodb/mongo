/**
 * Tests for bucket compression behavior when performing inserts to a time-series collection.
 * Normally, we only compress buckets when a bucket is full (closed). However, when the
 * TimeseriesAlwaysUseCompressedBuckets feature flag is on, an insert will insert a compressed
 * bucket into a time-series collection.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # We assume that all nodes in a mixed-mode replica set are using compressed inserts to
 *   # a time-series collection.
 *   requires_fcv_71,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

// Create a times-series collection.
let coll = db.getCollection(jsTestName());
coll.drop();  // Ensure that the namespace does not already exist, which can occur in some
              // burn_in suites.

const timeField = "time";
assert.commandWorked(db.createCollection(jsTestName(), {timeseries: {timeField: timeField}}));
coll = db.getCollection(jsTestName());

const bucketPrefix = "system.buckets.";
const bucketCollName = bucketPrefix + jsTestName();
const bucketColl = db.getCollection(bucketCollName);

/**
 * Test 1: Verify that an insert will create a compressed bucket in the time-series collection if
 * we are always using compressed buckets for time-series writes.
 *
 * If we are only compressing when the bucket is closed (i.e. the feature flag is not on), the
 * bucket document should reflect that it is not compressed.
 */
(function insertDocAndCheckCompressed() {
    jsTestLog("Entering insertDocAndCheckCompressed");
    assert.commandWorked(coll.insert({[timeField]: ISODate(), x: 0}));

    const bucketDoc = bucketColl.find().toArray()[0];
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        assert(TimeseriesTest.isBucketCompressed(bucketDoc.control.version),
               'Expected bucket to be compressed' + tojson(bucketDoc));
    } else {
        assert.eq(TimeseriesTest.BucketVersion.kUncompressed,
                  bucketDoc.control.version,
                  'Expected bucket not to be compressed' + tojson(bucketDoc));
    }

    jsTestLog("Exiting insertingDocCompressesBucket.");
})();

/**
 * Test 2: Verify that inserting a measurement that outside the [control.min.time,
 * control.max.time] range of the first bucket will create a new bucket.
 *
 * If we are only compressing when the bucket is closed (i.e. the feature flag is not on), the
 * bucket document should reflect that it is not compressed.
 */
(function targetNewBucketAndCheckCompressed() {
    jsTestLog("Entering targetNewBucketAndCheckCompressed...");

    // Because the measurement's timeField < the original bucket's control.min.time, this insert
    // should create a new bucket.
    assert.commandWorked(coll.insert({[timeField]: ISODate("2019-08-11T07:30:10.957Z"), x: 1}));

    const bucketDocs = bucketColl.find().toArray();
    assert.eq(2, bucketDocs.length, "Expected 2 buckets, but got " + bucketDocs.length + "buckets");

    // The buckets should be compressed if we are always using the compressed format for
    // time-series writes.
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        assert(TimeseriesTest.isBucketCompressed(bucketDocs[0].control.version),
               'Expected first bucket to be compressed' + tojson(bucketDocs));
        assert(TimeseriesTest.isBucketCompressed(bucketDocs[1].control.version),
               'Expected second bucket to be compressed' + tojson(bucketDocs));

        // Both buckets should contain 1 measurement.
        assert.eq(1,
                  bucketDocs[0].control.count,
                  "Expected 1 measurement in first bucket " + tojson(bucketDocs));
        assert.eq(1,
                  bucketDocs[1].control.count,
                  "Expected 1 measurement in second bucket " + tojson(bucketDocs));
    } else {
        assert.eq(TimeseriesTest.BucketVersion.kUncompressed,
                  bucketDocs[0].control.version,
                  'Expected first bucket not to be compressed' + tojson(bucketDocs));
        assert.eq(TimeseriesTest.BucketVersion.kUncompressed,
                  bucketDocs[1].control.version,
                  'Expected second bucket not to be compressed' + tojson(bucketDocs));
    }

    jsTestLog("Exiting targetNewBucketAndCheckCompressed.");
})();
