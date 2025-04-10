/**
 * Tests that the validate command checks data consistencies of the version field in time-series
 * bucket collections and return warnings properly.
 *
 * Version 1 indicates the bucket is uncompressed, and version 2 indicates the bucket is compressed.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

let testCount = 0;
const collNamePrefix = jsTestName();
let collName = collNamePrefix + testCount;
let coll = null;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

jsTestLog("Running the validate command to check time-series bucket versions");
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);

// Inserts documents into a bucket. Checks no issues are found.
TimeseriesTest.insertManyDocs(coll);

let res = coll.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// Inserts documents into another bucket but manually changes the version. Expects warnings
// from validation.
assert.eq(1,
          getTimeseriesCollForRawOps(db, coll)
              .find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
              .rawData()
              .count());
jsTestLog(
    "Manually changing 'control.version' from 2 to 1 and checking for warnings from validation.");
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
coll.insertMany([...Array(10).keys()].map(i => ({
                                              "metadata": {"sensorId": 2, "type": "temperature"},
                                              "timestamp": ISODate(),
                                              "temp": i
                                          })),
                {ordered: false});
getTimeseriesCollForRawOps(db, coll).updateOne(
    {"meta.sensorId": 2},
    {"$set": {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
    getRawOperationSpec(db));
res = coll.validate();
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

// Inserts enough documents to close a bucket and then manually changes the version to 1.
// Expects warnings from validation.
jsTestLog(
    "Changing the 'control.version' of a closed bucket from 2 to 1, and checking for warnings from validation.");
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
coll.insertMany([...Array(1200).keys()].map(i => ({
                                                "metadata": {"sensorId": 3, "type": "temperature"},
                                                "timestamp": ISODate(),
                                                "temp": i
                                            })),
                {ordered: false});
getTimeseriesCollForRawOps(db, coll).updateOne(
    {"meta.sensorId": 3, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
    {"$set": {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
    getRawOperationSpec(db));
res = coll.validate();
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

// Returns warnings on a bucket with an unsupported version.
jsTestLog("Changing 'control.version' to an unsupported version and checking for warnings.");
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
coll.insertMany([...Array(1100).keys()].map(i => ({
                                                "metadata": {"sensorId": 4, "type": "temperature"},
                                                "timestamp": ISODate(),
                                                "temp": i
                                            })),
                {ordered: false});
assert.gte(
    getTimeseriesCollForRawOps(db, coll)
        .find(
            {"meta.sensorId": 4, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
        .rawData()
        .count(),
    1);
getTimeseriesCollForRawOps(db, coll).updateOne(
    {"meta.sensorId": 4, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
    {"$set": {"control.version": 500}},
    getRawOperationSpec(db));
res = coll.validate();
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

// Creates a type-version mismatch in the previous bucket and checks that multiple warnings are
// reported from a single collection with multiple inconsistent documents.
jsTestLog(
    "Making a type-version mismatch in the same bucket as the previous test and checking for warnings.");
getTimeseriesCollForRawOps(db, coll).updateOne(
    {"meta.sensorId": 4, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
    {"$set": {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
    getRawOperationSpec(db));
res = coll.validate();
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 2);
assert.eq(res.errors.length, 1);

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, whereas before only a warning would be thrown.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
