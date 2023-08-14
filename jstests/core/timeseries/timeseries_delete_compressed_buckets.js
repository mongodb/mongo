/**
 * TODO SERVER-78946: Check if we can remove this test.
 * Tests running the deleteOne and deleteMany command on a time-series collection with compressed
 * buckets.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   requires_fcv_70,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
const numDocs = bucketMaxCount + 100;
const collNamePrefix = jsTestName() + "_";
let count = 0;
let coll;

function prepareCompressedBucket() {
    coll = db.getCollection(collNamePrefix + count++);
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    // Insert enough documents to trigger bucket compression.
    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({
            _id: i,
            [timeFieldName]: dateTime,
            [metaFieldName]: "meta",
            f: i,
            str: (i % 2 == 0 ? "even" : "odd")
        });
    }
    assert.commandWorked(coll.insert(docs));

    // Check the bucket collection to make sure that it generated the buckets we expect.
    const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();
    assert.eq(2, bucketDocs.length, tojson(bucketDocs));
    assert.eq(0,
              bucketDocs[0].control.min.f,
              "Expected first bucket to start at 0. " + tojson(bucketDocs));
    assert.eq(bucketMaxCount - 1,
              bucketDocs[0].control.max.f,
              `Expected first bucket to end at ${bucketMaxCount - 1}. ${tojson(bucketDocs)}`);
    assert.eq(2,
              bucketDocs[0].control.version,
              `Expected first bucket to be compressed. ${tojson(bucketDocs)}`);
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        // Version 2 indicates the bucket is compressed.
        assert.eq(2,
                  bucketDocs[1].control.version,
                  `Expected second bucket to be compressed. ${tojson(bucketDocs)}`);
    } else {
        // Version 1 indicates the bucket is uncompressed.
        assert.eq(1,
                  bucketDocs[1].control.version,
                  `Expected second bucket not to be compressed. ${tojson(bucketDocs)}`);
    }
    assert.eq(bucketMaxCount,
              bucketDocs[1].control.min.f,
              `Expected second bucket to start at ${bucketMaxCount}. ${tojson(bucketDocs)}`);
}

// Delete many records. This will hit both the compressed and uncompressed buckets.
prepareCompressedBucket();
let result = assert.commandWorked(coll.deleteMany({str: "even"}));
// TODO SERVER-77347: Check that the buckets stay compressed after a partial bucket deletion if the
// AlwaysUseCompressedBuckets feature flag is enabled.
assert.eq(numDocs / 2, result.deletedCount);
assert.eq(
    coll.countDocuments({str: "even"}), 0, "Expected records matching the filter to be deleted.");
assert.eq(coll.countDocuments({str: "odd"}),
          numDocs / 2,
          "Expected records not matching the filter not to be deleted.");

// Delete one record from the compressed bucket.
prepareCompressedBucket();
if (FeatureFlagUtil.isPresentAndEnabled(db, "UpdateOneWithoutShardKey")) {
    result = assert.commandWorked(coll.deleteOne({f: {$lt: 100}}));
    // TODO SERVER-77347: Check that the buckets stay compressed after a partial bucket deletion if
    // the AlwaysUseCompressedBuckets feature flag is enabled.
    assert.eq(1, result.deletedCount);
    assert.eq(coll.countDocuments({f: {$lt: 100}}),
              100 - 1,
              "Expected exactly one record matching the filter to be deleted.");
}