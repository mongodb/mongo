/**
 * Tests that the validate command checks for the equivalence of the timestamp embedded in the
 * time-series bucket document's '_id' field and the timestamp in the document's 'control.min.time'
 * field.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

let testCount = 0;
const collNamePrefix = jsTestName();
let collName = collNamePrefix + testCount;
let coll = null;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

jsTestLog(
    "Running the validate command to check time-series bucket OID timestamp and min timestamp equivalence.");
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);

// Inserts documents into a bucket. Checks no issues are found.
coll.insertMany(
    [...Array(10).keys()].map(i => ({
                                  "metadata": {"sensorId": testCount, "type": "temperature"},
                                  "timestamp": ISODate(),
                                  "temp": i
                              })),
    {ordered: false});
let res = coll.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// Inserts documents into another bucket but manually changes the min timestamp. Expects
// warnings from validation.
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
coll.insertMany(
    [...Array(10).keys()].map(i => ({
                                  "metadata": {"sensorId": testCount, "type": "temperature"},
                                  "timestamp": ISODate(),
                                  "temp": i
                              })),
    {ordered: false});
getTimeseriesCollForRawOps(db, coll).updateOne({"meta.sensorId": testCount},
                                               {"$set": {"control.min.timestamp": ISODate()}},
                                               getRawOperationSpec(db));

res = coll.validate();
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, whereas before only a warning would be thrown.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
