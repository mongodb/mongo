/**
 * Tests that the validate command checks for the equivalence of the timestamp embedded in the
 * time-series bucket document's '_id' field and the timestamp in the document's 'control.min.time'
 * field.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

let testCount = 0;
const collNamePrefix = "validate_timeseries_id_timestamp";
const bucketNamePrefix = "system.buckets.validate_timeseries_id_timestamp";
let collName = collNamePrefix + testCount;
let bucketName = bucketNamePrefix + testCount;
let coll = null;
let bucket = null;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

jsTestLog(
    "Running the validate command to check time-series bucket OID timestamp and min timestamp equivalence.");
testCount += 1;
collName = collNamePrefix + testCount;
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);

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
bucketName = bucketNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(
    collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
coll.insertMany(
    [...Array(10).keys()].map(i => ({
                                  "metadata": {"sensorId": testCount, "type": "temperature"},
                                  "timestamp": ISODate(),
                                  "temp": i
                              })),
    {ordered: false});
bucket.updateOne({"meta.sensorId": testCount}, {"$set": {"control.min.timestamp": ISODate()}});
res = coll.validate();

// TODO SERVER-87065: Validation should catch the timestamp error.
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.errors.length, 0);

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, whereas before only a warning would be thrown.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
