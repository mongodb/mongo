/**
 * Tests running the deleteOne and deleteMany command on a time-series collection with compressed
 * buckets.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   requires_fcv_70,
 *   # This test depends on certain writes ending up in the same bucket to trigger compression.
 *   # Stepdowns may result in writes splitting between two primaries, and
 *   # thus different buckets.
 *   does_not_support_stepdowns,
 *   # The config fuzzer can fuzz the bucketMaxCount.
 *   does_not_support_config_fuzzer,
 * ]
 */

import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
const numDocs = bucketMaxCount + 100;
const collNamePrefix = jsTestName() + "_";
let count = 0;
let coll;

function assertBucketsAreCompressed(db, coll) {
    const bucketDocs = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    bucketDocs.forEach((bucketDoc) => {
        assert(
            TimeseriesTest.isBucketCompressed(bucketDoc.control.version),
            `Expected bucket to be compressed: ${tojson(bucketDoc)}`,
        );
    });
}

function prepareCompressedBucket() {
    coll = db.getCollection(collNamePrefix + count++);
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    // Insert enough documents to trigger bucket compression.
    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({
            _id: i,
            [timeFieldName]: dateTime,
            [metaFieldName]: "meta",
            f: i,
            str: i % 2 == 0 ? "even" : "odd",
        });
    }
    assert.commandWorked(coll.insert(docs));

    // Check the buckets to make sure it generated what we expect.
    const bucketDocs = getTimeseriesCollForRawOps(coll).find().rawData().sort({"control.min._id": 1}).toArray();
    assert.eq(2, bucketDocs.length, tojson(bucketDocs));
    assert.eq(0, bucketDocs[0].control.min.f, "Expected first bucket to start at 0. " + tojson(bucketDocs));
    assert.eq(
        bucketMaxCount - 1,
        bucketDocs[0].control.max.f,
        `Expected first bucket to end at ${bucketMaxCount - 1}. ${tojson(bucketDocs)}`,
    );
    assert(
        TimeseriesTest.isBucketCompressed(bucketDocs[0].control.version),
        `Expected first bucket to be compressed. ${tojson(bucketDocs)}`,
    );
    assert(
        TimeseriesTest.isBucketCompressed(bucketDocs[1].control.version),
        `Expected second bucket to be compressed. ${tojson(bucketDocs)}`,
    );
    assert.eq(
        bucketMaxCount,
        bucketDocs[1].control.min.f,
        `Expected second bucket to start at ${bucketMaxCount}. ${tojson(bucketDocs)}`,
    );
}

// Delete many records. This will hit both the compressed and uncompressed buckets.
prepareCompressedBucket();
let result = assert.commandWorked(coll.deleteMany({str: "even"}));
assert.eq(numDocs / 2, result.deletedCount);
assert.eq(coll.countDocuments({str: "even"}), 0, "Expected records matching the filter to be deleted.");
assert.eq(
    coll.countDocuments({str: "odd"}),
    numDocs / 2,
    "Expected records not matching the filter not to be deleted.",
);
assertBucketsAreCompressed(db, coll);

// Delete one record from the compressed bucket.
prepareCompressedBucket();
result = assert.commandWorked(coll.deleteOne({f: {$lt: 100}}));
assert.eq(1, result.deletedCount);
assert.eq(
    coll.countDocuments({f: {$lt: 100}}),
    100 - 1,
    "Expected exactly one record matching the filter to be deleted.",
);
assertBucketsAreCompressed(db, coll);
