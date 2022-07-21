/**
 * Tests that the validate command now checks that 'control.min' and 'control.max' fields in a
 * time-series bucket agree with those in 'data', and that it adds a warning and
 * increments the number of noncompliant documents if they don't.
 * @tags: [featureFlagExtendValidateCommand]
 */

(function() {
"use strict";

const collPrefix = "validate_timeseries_minmax";
const bucketPrefix = "system.buckets.validate_timeseries_minmax";
let collName = "validate_timeseries_minmax";
let bucketName = "system.buckets.validate_timeseries_minmax";
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
        "temp": 15
    },
];

const weatherDataWithGap = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 15
    },
];

const stringData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
        "str": "a"
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "temp": 16,
        "str": "11"
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 16,
        "str": "18"
    },
];

const arrayData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "arr": ["a", {"field": 1}, 1]
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "arr": ["1", {"field": 2}, 100]
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "arr": ["z", {"field": -2}, 30]
    },
];

const objectData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "obj": {"nestedObj": {"field": 0}},
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "obj": {"nestedObj": {"field": 10}},
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "obj": {"nestedObj": {"field": 2}},
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
    let collection = db.getCollection(collName);
    assert.commandWorked(collection.insertMany(data));
    let result = assert.commandWorked(collection.validate());
    assert(result.valid, tojson(result));
    assert(result.warnings.length == 0, tojson(result));
    assert(result.nNonCompliantDocuments == 0, tojson(result));
}

// Updates 'control' min temperature with corresponding update in recorded temperature, testing
// that valid updates do not return warnings.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with correct 'max' temperature, using collection " +
          collName + ".");
let coll = db.getCollection(collName);
let bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$set": {"control.max.temp": 17}}));
assert.commandWorked(bucket.update({}, {"$set": {"data.temp.1": 17}}));
let res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 0, tojson(res));
assert(res.nNonCompliantDocuments == 0, tojson(res));

// Updates the 'control' min and max temperature without an update in recorded temperature.
setUpCollection(weatherData);
jsTestLog(
    "Running validate on bucket with incorrect 'min' and 'max' temperature, using collection " +
    collName + ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$set": {"control.min.temp": -200}}));
assert.commandWorked(bucket.update({}, {"$set": {"control.max.temp": 500}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Updates the 'control' min and max temperature without an update in recorded temperature, when
// there is also a gap in observed temperature (i.e, one or more of the data entries does not have a
// 'temp' field).
setUpCollection(weatherDataWithGap);
jsTestLog(
    "Running validate on bucket with incorrect 'min' and 'max' temperature with gaps in temperature data, using collection " +
    collName + ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$set": {"control.min.temp": -200}}));
assert.commandWorked(bucket.update({}, {"$set": {"control.max.temp": 500}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Updates the 'control' max _id without an update in recorded temperature.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with incorrect 'max' '_id', using collection " + collName +
          ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(
    bucket.update({}, {"$set": {"control.max._id": ObjectId("62bcc728f3c51f43297eea43")}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Updates the 'control' min timestamp without an update in recorded temperature.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with incorrect 'min' timestamp, using collection " +
          collName + ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(
    bucket.update({}, {"$set": {"control.min.timestamp": ISODate("2021-05-19T13:00:00.000Z")}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Adds an extra field to the 'control' min object.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with extra control field, using collection " + collName +
          ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$set": {"control.min.extra": 10}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests whether discrepancies with string min/max values are caught.
setUpCollection(stringData);
jsTestLog(
    "Running validate on bucket with incorrect 'min' and 'max' string field, using collection " +
    collName + ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$set": {"control.min.str": "-200"}}));
assert.commandWorked(bucket.update({}, {"$set": {"control.max.str": "zzz"}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests whether discrepancies with array values are caught.
setUpCollection(arrayData);
jsTestLog("Running validate on bucket with incorrect 'max' array field, using collection " +
          collName + ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(
    bucket.update({}, {"$set": {"control.max.arr": ["zzzzz", {"field": -2}, 30]}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests whether discrepancies with nested objects are caught.
setUpCollection(objectData);
jsTestLog("Running validate on bucket with incorrect 'max' object field, using collection " +
          collName + ".");
coll = db.getCollection(collName);
bucket = db.getCollection(bucketName);
assert.commandWorked(bucket.update({}, {"$set": {"control.max.obj": {"nestedObj": {"field": 2}}}}));
res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));
})();