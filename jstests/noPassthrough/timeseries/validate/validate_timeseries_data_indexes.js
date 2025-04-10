/**
 * Tests that the validate command now checks the indexes in the time-series buckets data fields.
 *
 * @tags: [requires_fcv_62]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const collPrefix = jsTestName();
let collName = collPrefix;
let testCount = 0;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const weatherData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "temp": 16
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "humidity": 15
    },
];

// Drops collection and creates it again with new data, checking that collection is valid before
// faulty data is inserted.
function setUpCollection(data) {
    testCount += 1;
    collName = collPrefix + testCount;
    db.getCollection(collName).drop();
    assert.commandWorked(db.createCollection(
        collName,
        {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}));
    const collection = db.getCollection(collName);
    assert.commandWorked(collection.insertMany(data));
    const result = assert.commandWorked(collection.validate());
    assert(result.valid, tojson(result));
    assert(result.warnings.length == 0, tojson(result));
    assert(result.nNonCompliantDocuments == 0, tojson(result));

    // If we are always writing to time-series collections using the compressed format, replace the
    // compressed bucket with the decompressed bucket. This allows this test to directly make
    // updates to bucket measurements in order to test validation.
    const bucketDoc = getTimeseriesCollForRawOps(db, collection).find().rawData()[0];
    TimeseriesTest.decompressBucket(bucketDoc);
    getTimeseriesCollForRawOps(db, collection)
        .replaceOne({_id: bucketDoc._id}, bucketDoc, getRawOperationSpec(db));
}

// Non-numerical index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with non-numerical data field indexes on collection " +
          collName);
let coll = db.getCollection(collName);
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {}, {"$rename": {"data.temp.0": "data.temp.a"}}, getRawOperationSpec(db)));
let res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Non-increasing index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with non-increasing data field indexes on collection " +
          collName);
coll = db.getCollection(collName);
// This keeps indexes from being reordered by Javascript.
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {},
    {"$rename": {"data.temp.0": "data.temp.a", "data.temp.1": "data.temp.b"}},
    getRawOperationSpec(db)));
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {}, {"$rename": {"data.temp.a": "data.temp.1"}}, getRawOperationSpec(db)));
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {}, {"$rename": {"data.temp.b": "data.temp.0"}}, getRawOperationSpec(db)));
printjson(getTimeseriesCollForRawOps(db, coll).find().rawData().toArray());
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Out-of-range index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with out-of-range data field indexes on collection " +
          collName);
coll = db.getCollection(collName);
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {}, {"$rename": {"data.temp.1": "data.temp.10"}}, getRawOperationSpec(db)));
printjson(getTimeseriesCollForRawOps(db, coll).find().rawData().toArray());
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Negative index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with negative data field indexes on collection " + collName);
coll = db.getCollection(collName);
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {}, {"$rename": {"data.temp.0": "data.temp.-1"}}, getRawOperationSpec(db)));
printjson(getTimeseriesCollForRawOps(db, coll).find().rawData().toArray());
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Missing time field index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with missing time field indexes on collection " + collName);
coll = db.getCollection(collName);
assert.commandWorked(getTimeseriesCollForRawOps(db, coll).update(
    {}, {"$unset": {"data.timestamp.1": ""}}, getRawOperationSpec(db)));
printjson(getTimeseriesCollForRawOps(db, coll).find().rawData().toArray());
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, whereas before only a warning would be thrown.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
