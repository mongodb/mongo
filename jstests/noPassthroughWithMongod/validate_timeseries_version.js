/**
 * Tests that the validate command checks data consistencies of the version field in time-series
 * bucket collections and return warnings properly.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

(function() {
"use strict";
let testCount = 0;
const collNamePrefix = "validate_timeseries_version";
const bucketNamePrefix = "system.buckets.validate_timeseries_version";
let collName = collNamePrefix + testCount;
let bucketName = bucketNamePrefix + testCount;
let coll = null;
let bucket = null;

jsTestLog("Running the validate command to check time-series bucket versions");
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
coll.insertMany([...Array(10).keys()].map(
    i =>
        ({"metadata": {"sensorId": 1, "type": "temperature"}, "timestamp": ISODate(), "temp": i})));
let res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// Inserts documents into another bucket but manually changes the version to 2. Expects warnings
// from validation.
jsTestLog(
    "Manually changing 'control.version' from 1 to 2 and checking for warnings from validation.");
testCount += 1;
collName = collNamePrefix + testCount;
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
coll.insertMany([...Array(10).keys()].map(
    i =>
        ({"metadata": {"sensorId": 2, "type": "temperature"}, "timestamp": ISODate(), "temp": i})));
bucket.updateOne({"meta.sensorId": 2}, {"$set": {"control.version": 2}});
res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.warnings.length, 1);

// Inserts enough documents to close a bucket and then manually changes the version to 1.
// Expects warnings from validation.
jsTestLog(
    "Changing the 'control.version' of a closed bucket from 2 to 1, and checking for warnings from validation.");
testCount += 1;
collName = collNamePrefix + testCount;
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
coll.insertMany([...Array(1200).keys()].map(
    i =>
        ({"metadata": {"sensorId": 3, "type": "temperature"}, "timestamp": ISODate(), "temp": i})));
bucket.updateOne({"meta.sensorId": 3, "control.version": 2}, {"$set": {"control.version": 1}});
res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.warnings.length, 1);

// Returns warnings on a bucket with an unsupported version.
jsTestLog("Changing 'control.version' to an unsupported version and checking for warnings.");
testCount += 1;
collName = collNamePrefix + testCount;
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
coll.insertMany([...Array(1100).keys()].map(
    i =>
        ({"metadata": {"sensorId": 4, "type": "temperature"}, "timestamp": ISODate(), "temp": i})));
bucket.updateOne({"meta.sensorId": 4, "control.version": 2}, {"$set": {"control.version": 500}});
res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.warnings.length, 1);

// Creates a type-version mismatch in the previous bucket and checks that multiple warnings are
// reported from a single collection with multiple inconsistent documents.
jsTestLog(
    "Making a type-version mismatch in the same bucket as the previous test and checking for warnings.");
bucket.updateOne({"meta.sensorId": 4, "control.version": 1}, {"$set": {"control.version": 2}});
res = bucket.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 2);
assert.eq(res.warnings.length, 1);
})();