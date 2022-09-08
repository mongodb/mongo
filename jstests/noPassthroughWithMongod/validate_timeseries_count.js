/**
 * Tests that the validate command checks that the number of measurements in a time-series
 * collection matches the 'control.count' field.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

(function() {
"use strict";
let testCount = 0;
const collNamePrefix = "validate_timeseries_count";
const bucketNamePrefix = "system.buckets.validate_timeseries_count";
let collName = collNamePrefix + testCount;
let bucketName = bucketNamePrefix + testCount;
let coll = null;
let bucket = null;

jsTestLog(
    "Running the validate command to check that time-series bucket 'control.count' matches the number of measurements in version-2 buckets.");
testCount += 1;
collName = collNamePrefix + testCount;
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);

// Inserts documents into a bucket. Checks no issues are found.
jsTestLog("Inserting documents into a bucket and checking that no issues are found.");
coll.insertMany([...Array(1010).keys()].map(
    i =>
        ({"metadata": {"sensorId": 1, "type": "temperature"}, "timestamp": ISODate(), "temp": i})));
let res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// Manually changes the control.count of a version-2 (closed) bucket, expects warnings.
jsTestLog("Manually changing the 'control.count' of a version-2 bucket.");
testCount += 1;
collName = collNamePrefix + testCount;
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
coll.insertMany([...Array(1002).keys()].map(
    i =>
        ({"metadata": {"sensorId": 2, "type": "temperature"}, "timestamp": ISODate(), "temp": i})));
bucket.updateOne({"meta.sensorId": 2, 'control.version': 2}, {"$set": {"control.count": 10}});
res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.warnings.length, 1);
})();