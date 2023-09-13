/**
 * Tests that an update on a time-series collection keeps the buckets compressed on disk when the
 * feature flag TimeseriesAlwaysUseCompressedBuckets is enabled.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   tenant_migration_incompatible,
 *   featureFlagTimeseriesAlwaysUseCompressedBuckets,
 *   requires_fcv_71,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

jsTestLog("Running " + jsTestName());

const timeField = "time";
const kBucketMax = 1000;
// Create the time-series collection.
assert.commandWorked(db.createCollection(jsTestName(), {timeseries: {timeField: timeField}}));
const coll = db.getCollection(jsTestName());
const bucketsColl = db.getCollection("system.buckets." + jsTestName());

function insertAndCheckBuckets(value) {
    assert.commandWorked(coll.insert({[timeField]: ISODate(), x: value}));
    let buckets = bucketsColl.find().toArray();
    buckets.forEach((bucket, index) => {
        assert.eq(
            bucket.control.version,
            TimeseriesTest.BucketVersion.kCompressed,
            `Bucket ${index} does not have the correct version. Expected ${
                TimeseriesTest.BucketVersion.kCompressed}, but got ${bucket.control.version}`);
    });
}

// The first insert and 1 - kBucketMax subsequent updates should all go to the same compressed
// bucket.
for (let i = 0; i < kBucketMax; i++) {
    insertAndCheckBuckets(i);
}

let buckets = bucketsColl.find().toArray();
assert.eq(buckets.length, 1, `Expected 1 bucket, but got ${buckets.length}: ${tojson(buckets)}`);

// The full bucket should be closed and a future measurement should go to another bucket.
insertAndCheckBuckets(kBucketMax);
buckets = bucketsColl.find().toArray();
assert.eq(buckets.length, 2, `Expected 2 buckets, but got ${buckets.length}: ${tojson(buckets)}`);
