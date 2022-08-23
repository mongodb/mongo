/**
 * Tests that the validate command now checks the indexes in the time-series buckets data fields.
 *
 * @tags: [featureFlagExtendValidateCommand]
 */

(function() {
"use strict";

const collPrefix = "validate_timeseries_data_indexes";
const bucketPrefix = "system.buckets.validate_timeseries_data_indexes";
let collName = collPrefix;
let bucketName = bucketPrefix;
let testCount = 0;

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
    bucketName = bucketPrefix + testCount;
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
}

// Non-numerical index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with non-numerical data field indexes on collection " +
          collName);
let coll = db.getCollection(collName);
let bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$rename": {"data.temp.0": "data.temp.a"}}));
let res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Non-increasing index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with non-increasing data field indexes on collection " +
          collName);
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
// This keeps indexes from being reordered by Javascript.
assert.commandWorked(
    bucket.update({}, {"$rename": {"data.temp.0": "data.temp.a", "data.temp.1": "data.temp.b"}}));
assert.commandWorked(bucket.update({}, {"$rename": {"data.temp.a": "data.temp.1"}}));
assert.commandWorked(bucket.update({}, {"$rename": {"data.temp.b": "data.temp.0"}}));
printjson(bucket.find().toArray());
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Out-of-range index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with out-of-range data field indexes on collection " +
          collName);
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$rename": {"data.temp.1": "data.temp.10"}}));
printjson(bucket.find().toArray());
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Negative index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with negative data field indexes on collection " + collName);
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$rename": {"data.temp.0": "data.temp.-1"}}));
printjson(bucket.find().toArray());
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Missing time field index.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with missing time field indexes on collection " + collName);
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$unset": {"data.timestamp.1": ""}}));
printjson(bucket.find().toArray());
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));
})();