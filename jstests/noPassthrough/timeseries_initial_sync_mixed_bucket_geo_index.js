/**
 * Verifies that a 2dsphere time-series index correctly generates keys for buckets whose
 * control.version field disagrees with the actual storage format of individual data columns.
 *
 * Such "mixed compression" buckets can arise during logical initial sync when a secondary clones a
 * compressed (v2) bucket and then replays oplog operations from an earlier uncompressed
 * state, or vice versa. This test creates the mixed state directly by writing to system.buckets
 * and verifies that index builds still produce correct key counts. See SERVER-121132 for details.
 *
 * @tags: [
 *   requires_fcv_70,
 *   requires_timeseries,
 * ]
 */

(function() {
"use strict";

const kBaseTime = ISODate("2024-01-01T00:00:00Z");
const kOneSecondMs = 1000;
const kTwoHoursMs = 2 * 60 * 60 * 1000;
const kGeoPoint = {
    type: "Point",
    coordinates: [0, 0]
};

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("test");
const collName = jsTestName() + "_ts";
const coll = testDB.getCollection(collName);
const bucketsColl = testDB.getCollection("system.buckets." + collName);

assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));

// Two measurements in the same bucket, then a third >1 hour later to close and compress it.
assert.commandWorked(coll.insertMany([
    {t: kBaseTime, m: "meta", location: kGeoPoint},
    {t: new Date(kBaseTime.getTime() + kOneSecondMs), m: "meta", location: kGeoPoint},
    {t: new Date(kBaseTime.getTime() + kTwoHoursMs), m: "meta", location: kGeoPoint},
]));

const v2Bucket = bucketsColl.findOne({"control.version": 2});
assert.neq(null,
           v2Bucket,
           "Expected a compressed (v2) bucket. Got: " + tojson(bucketsColl.find().toArray()));

assert.commandWorked(coll.createIndex({location: "2dsphere"}));

// Create a "mixed compression" bucket: version says v1 but data columns are compressed BinData.
assert.commandWorked(
    bucketsColl.update({_id: v2Bucket._id}, {$set: {"control.version": NumberInt(1)}}));

// Rebuild the index over the mixed bucket.
assert.commandWorked(coll.dropIndex({location: "2dsphere"}));
assert.commandWorked(coll.createIndex({location: "2dsphere"}));

// Restore version=2 and validate. Validate regenerates keys using the correct version; if the
// index build produced wrong keys, they will be detected as missing entries.
assert.commandWorked(
    bucketsColl.update({_id: v2Bucket._id}, {$set: {"control.version": NumberInt(2)}}));

const result = bucketsColl.validate({full: true});
assert(result.valid, "Index built on bucket is missing keys. " + tojson(result));

MongoRunner.stopMongod(conn);
})();
