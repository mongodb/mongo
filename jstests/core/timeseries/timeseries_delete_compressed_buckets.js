/**
 * Tests running the deleteOne and deleteMany command on a time-series collection with compressed
 * buckets.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesDeletesSupport,
 * ]
 */

(function() {
"use strict";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
const numDocs = bucketMaxCount + 100;

const coll = db.getCollection(jsTestName());
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
assert.eq(
    0, bucketDocs[0].control.min.f, "Expected first bucket to start at 0. " + tojson(bucketDocs));
assert.eq(bucketMaxCount - 1,
          bucketDocs[0].control.max.f,
          `Expected first bucket to end at ${bucketMaxCount - 1}. ${tojson(bucketDocs)}`);
assert.eq(2,
          bucketDocs[0].control.version,
          `Expected first bucket to be compressed. ${tojson(bucketDocs)}`);
assert.eq(1,
          bucketDocs[1].control.version,
          `Expected second bucket not to be compressed. ${tojson(bucketDocs)}`);
assert.eq(bucketMaxCount,
          bucketDocs[1].control.min.f,
          `Expected second bucket to start at ${bucketMaxCount}. ${tojson(bucketDocs)}`);

// Delete many records. This will hit both the compressed and uncompressed buckets.
let result = assert.commandWorked(coll.deleteMany({str: "even"}));
assert.eq(numDocs / 2, result.deletedCount);
assert.eq(
    coll.countDocuments({str: "even"}), 0, "Expected records matching the filter to be deleted.");
assert.eq(coll.countDocuments({str: "odd"}),
          numDocs / 2,
          "Expected records not matching the filter not to be deleted.");

// Delete one record from the compressed bucket.
result = assert.commandWorked(coll.deleteOne({f: {$lt: 100}}));
assert.eq(1, result.deletedCount);
assert.eq(coll.countDocuments({f: {$lt: 100}}),
          100 - 50 - 1,  // 100 records to start + 50 deleted above + 1 more deleted
          "Expected records matching the filter to be deleted.");
})();
