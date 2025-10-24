/**
 * Tests directly inserting a time-series bucket with mixed schema.
 *
 * @tags: [
 *   requires_timeseries,
 *   # TODO(SERVER-108445) Reenable this test
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

TestData.skipEnforceTimeseriesBucketsAreAlwaysCompressedOnValidate = true;

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
const coll = testDB[collName];
const bucketsColl = testDB["system.buckets." + collName];

let doc = {t: ISODate(), m: "meta", a: 1, payload: "small"};
assert.commandWorked(coll.insert(doc));
assert.eq(1, coll.find({"m": "meta"}).toArray().length);
assert.eq(1, bucketsColl.find({}).toArray().length);

// new measurement, because schema changed, we have a new bucket
// previous bug would skip mixed schema check due to handling of large measurements
doc.a = "foo";
doc.payload = "A".repeat(130000);
assert.commandWorked(coll.insert(doc));
assert.eq(2, coll.find({"m": "meta"}).toArray().length);
assert.eq(2, bucketsColl.find({"meta": "meta"}).toArray().length);
let schemaChangedBucket = bucketsColl.find({"control.min.a": "foo"}).toArray();
assert.eq(1, schemaChangedBucket.length);
let schemaChangedBucketId = schemaChangedBucket[0]._id;

// new measurement, schema changed back, necessitating a different bucket
doc.a = 2;
doc.payload = "small";
assert.commandWorked(coll.insert(doc));
assert.eq(3, coll.find({"m": "meta"}).toArray().length);
// New measurement may go into new bucket, or prev bucket (reopened), but should not go into
// the recently created bucket.
schemaChangedBucket = bucketsColl.find({"_id": schemaChangedBucketId}).toArray();
assert.eq(1, schemaChangedBucket.length);
assert.neq(2, schemaChangedBucket[0].control.min.a);
assert.neq(2, schemaChangedBucket[0].control.max.a);
})();
