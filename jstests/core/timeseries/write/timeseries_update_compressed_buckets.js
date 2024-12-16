/**
 * Tests running the updateOne and updateMany command on a time-series collection with compressed
 * buckets.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # This test depends on certain writes ending up in the same bucket to trigger compression.
 *   # Stepdowns may result in writes splitting between two primaries, and
 *   # thus different buckets.
 *   does_not_support_stepdowns,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2023-07-13T16:00:00Z");

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
const numDocs = bucketMaxCount + 100;
const collNamePrefix = jsTestName() + "_";
let count = 0;
let coll;
let bucketsColl;

function assertBucketsAreCompressed(db, bucketsColl) {
    const bucketDocs = bucketsColl.find().toArray();
    bucketDocs.forEach(bucketDoc => {
        assert(TimeseriesTest.isBucketCompressed(bucketDoc.control.version),
               `Expected bucket to be compressed: ${tojson(bucketDoc)}`);
    });
}

function prepareCompressedBucket() {
    coll = db.getCollection(collNamePrefix + count++);
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    bucketsColl = db.getCollection('system.buckets.' + coll.getName());

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
    if (!TestData.runningWithBalancer) {
        assert.eq(2, bucketDocs.length, tojson(bucketDocs));
        assert.eq(0,
                  bucketDocs[0].control.min.f,
                  "Expected first bucket to start at 0. " + tojson(bucketDocs));
        assert.eq(bucketMaxCount - 1,
                  bucketDocs[0].control.max.f,
                  `Expected first bucket to end at ${bucketMaxCount - 1}. ${tojson(bucketDocs)}`);
        assert(TimeseriesTest.isBucketCompressed(bucketDocs[0].control.version),
               `Expected first bucket to be compressed. ${tojson(bucketDocs)}`);
        assert(TimeseriesTest.isBucketCompressed(bucketDocs[1].control.version),
               `Expected second bucket to be compressed. ${tojson(bucketDocs)}`);
        assert.eq(bucketMaxCount,
                  bucketDocs[1].control.min.f,
                  `Expected second bucket to start at ${bucketMaxCount}. ${tojson(bucketDocs)}`);
    } else {
        // If we are running with moveCollection in the background, we may run into the issue
        // described by SERVER-89349 which can result in more bucket documents than needed.
        // However, we still want to check that the number of documents is within the acceptable
        // range.
        assert.lte(2, bucketDocs.length, tojson(bucketDocs));
        let currMin = 0;
        bucketDocs.forEach((doc) => {
            assert.eq(currMin,
                      doc.control.min.f,
                      `Expected bucket to start at ${currMin}. ${tojson(bucketDocs)}`);
            assert(TimeseriesTest.isBucketCompressed(doc.control.version));
            currMin = doc.control.max.f + 1;
        });
    }
}

// Update many records. This will hit both the compressed and uncompressed buckets.
prepareCompressedBucket();
let result = assert.commandWorked(coll.updateMany({str: "even"}, {$inc: {updated: 1}}));
assert.eq(numDocs / 2, result.modifiedCount);
assert.eq(coll.countDocuments({updated: 1, str: "even"}),
          numDocs / 2,
          "Expected records matching the filter to be updated.");
assert.eq(coll.countDocuments({updated: 1, str: "odd"}),
          0,
          "Expected records not matching the filter not to be updated.");
assertBucketsAreCompressed(db, bucketsColl);

// Update one record from the compressed bucket.
prepareCompressedBucket();
result = assert.commandWorked(coll.updateOne({str: "even", f: {$lt: 100}}, {$inc: {updated: 1}}));
assert.eq(1, result.modifiedCount);
assert.eq(coll.countDocuments({updated: 1, str: "even", f: {$lt: 100}}),
          1,
          "Expected exactly one record matching the filter to be updated.");
assert.eq(coll.countDocuments({updated: 1}),
          1,
          "Expected records not matching the filter not to be updated.");
assertBucketsAreCompressed(db, bucketsColl);
