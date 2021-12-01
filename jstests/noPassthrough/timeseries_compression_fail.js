/**
 * Tests that the server can detect when timeseries bucket compression is not decompressible without
 * data loss. Bucket should remain uncompressed and we log that this happened.
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

let conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);
const coll = db.getCollection('t');

// Only run test when bucket compression is enabled
if (!FeatureFlagUtil.isEnabled(db, "TimeseriesBucketCompression")) {
    MongoRunner.stopMongod(conn);
    return;
}

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
const numDocs = bucketMaxCount + 100;

// Simulate compression data loss by enabling fail point
assert.commandWorked(db.adminCommand(
    {configureFailPoint: "simulateBsonColumnCompressionDataLoss", mode: "alwaysOn"}));

// Create timeseries collection
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));

const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

// Insert enough documents to trigger bucket compression
for (let i = 0; i < numDocs; i++) {
    let doc = {_id: i, time: ISODate(), x: i};
    assert.commandWorked(coll.insert(doc), 'failed to insert docs: ' + tojson(doc));
}

// Check for "Time-series bucket compression failed due to decompression data loss" log entry
checkLog.containsJson(conn, 6179301);

// Check for "numFailedDecompressBuckets" in collStats
const stats = assert.commandWorked(coll.stats());
assert(stats.timeseries.hasOwnProperty('numFailedDecompressBuckets'));
assert.gt(stats.timeseries["numFailedDecompressBuckets"], 0);

// Check that we did not compress the bucket
const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();
assert.eq(2, bucketDocs.length, tojson(bucketDocs));
assert.eq(1, bucketDocs[0].control.version);
assert.eq(1, bucketDocs[1].control.version);

MongoRunner.stopMongod(conn);
})();
