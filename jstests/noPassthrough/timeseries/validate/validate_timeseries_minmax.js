/**
 * Tests that the validate command now checks that 'control.min' and 'control.max' fields in a
 * time-series bucket agree with those in 'data', and that it adds a warning and
 * increments the number of noncompliant documents if they don't.
 * @tags: [requires_fcv_62]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const collPrefix = jsTestName();
let collName = jsTestName();
let testCount = 0;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const weatherData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "temp": 16,
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 15,
    },
];

const weatherDataWithGap = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 15,
    },
];

const stringData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
        "str": "a",
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "temp": 16,
        "str": "11",
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 16,
        "str": "18",
    },
];

const arrayData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "arr": ["a", {"field": 1}, 1],
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "arr": ["1", {"field": 2}, 100],
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "arr": ["z", {"field": -2}, 30],
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

const lotsOfData = [...Array(1010).keys()].map((i) => ({
    "metadata": {"sensorId": 2, "type": "temperature"},
    "timestamp": ISODate(),
    "temp": i,
}));

const skipFieldData = [...Array(1010).keys()].map(function (i) {
    if (i % 2) {
        return {"timestamp": ISODate(), "temp": i};
    } else {
        return {"timestamp": ISODate()};
    }
});

// Drops collection and creates it again with new data, checking that collection is valid before
// faulty data is inserted.
function setUpCollection(data) {
    testCount += 1;
    collName = collPrefix + testCount;
    db.getCollection(collName).drop();
    assert.commandWorked(
        db.createCollection(collName, {
            timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
        }),
    );
    let collection = db.getCollection(collName);
    assert.commandWorked(collection.insertMany(data, {ordered: false}));

    // If we are always writing to time-series collections using the compressed format, replace the
    // compressed bucket with the decompressed bucket.
    const bucketDoc = getTimeseriesCollForRawOps(db, collection).find().rawData()[0];
    TimeseriesTest.decompressBucket(bucketDoc);
    getTimeseriesCollForRawOps(db, collection).replaceOne({_id: bucketDoc._id}, bucketDoc, getRawOperationSpec(db));

    let result = assert.commandWorked(collection.validate());
    assert(result.valid, tojson(result));
    assert(result.warnings.length == 0, tojson(result));
    assert(result.nNonCompliantDocuments == 0, tojson(result));
}

// Updates 'control' min temperature with corresponding update in recorded temperature, testing
// that valid updates do not return warnings.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with correct 'max' temperature, using collection " + collName + ".");
let coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.max.temp": 17}}, getRawOperationSpec(db)),
);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"data.temp.1": 17}}, getRawOperationSpec(db)),
);
let res = assert.commandWorked(coll.validate());
assert(res.valid, tojson(res));
assert(res.warnings.length == 0, tojson(res));
assert(res.nNonCompliantDocuments == 0, tojson(res));

// Updates the 'control' min and max temperature without an update in recorded temperature.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with incorrect 'min' and 'max' temperature, using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.min.temp": -200}}, getRawOperationSpec(db)),
);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.max.temp": 500}}, getRawOperationSpec(db)),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Updates the 'control' min and max temperature without an update in recorded temperature, when
// there is also a gap in observed temperature (i.e, one or more of the data entries does not have a
// 'temp' field).
setUpCollection(weatherDataWithGap);
jsTestLog(
    "Running validate on bucket with incorrect 'min' and 'max' temperature with gaps in temperature data, using collection " +
        collName +
        ".",
);
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.min.temp": -200}}, getRawOperationSpec(db)),
);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.max.temp": 500}}, getRawOperationSpec(db)),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Updates the 'control' max _id without an update in recorded temperature.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with incorrect 'max' '_id', using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update(
        {},
        {"$set": {"control.max._id": ObjectId("62bcc728f3c51f43297eea43")}},
        getRawOperationSpec(db),
    ),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Updates the 'control' min timestamp without an update in recorded temperature.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with incorrect 'min' timestamp, using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update(
        {},
        {"$set": {"control.min.timestamp": ISODate("2021-05-19T13:00:00.000Z")}},
        getRawOperationSpec(db),
    ),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Adds an extra field to the 'control' min object.
setUpCollection(weatherData);
jsTestLog("Running validate on bucket with extra control field, using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.min.extra": 10}}, getRawOperationSpec(db)),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 2, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests whether discrepancies with string min/max values are caught.
setUpCollection(stringData);
jsTestLog("Running validate on bucket with incorrect 'min' and 'max' string field, using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.min.str": "-200"}}, getRawOperationSpec(db)),
);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update({}, {"$set": {"control.max.str": "zzz"}}, getRawOperationSpec(db)),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests whether discrepancies with array values are caught.
setUpCollection(arrayData);
jsTestLog("Running validate on bucket with incorrect 'max' array field, using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update(
        {},
        {"$set": {"control.max.arr": ["zzzzz", {"field": -2}, 30]}},
        getRawOperationSpec(db),
    ),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests whether discrepancies with nested objects are caught.
setUpCollection(objectData);
jsTestLog("Running validate on bucket with incorrect 'max' object field, using collection " + collName + ".");
coll = db.getCollection(collName);
assert.commandWorked(
    getTimeseriesCollForRawOps(db, coll).update(
        {},
        {"$set": {"control.max.obj": {"nestedObj": {"field": 2}}}},
        getRawOperationSpec(db),
    ),
);
res = assert.commandWorked(coll.validate());
assert(!res.valid, tojson(res));
assert(res.errors.length == 1, tojson(res));
assert(res.nNonCompliantDocuments == 1, tojson(res));

// Tests collections with 'control.version' : 2, which represents compressed buckets
jsTestLog("Running validate on a version 2 bucket with incorrect 'max' object field.");
setUpCollection(lotsOfData);
coll = db.getCollection(collName);
getTimeseriesCollForRawOps(db, coll).updateOne(
    {"meta.sensorId": 2, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
    {"$set": {"control.max.temp": 800}},
    getRawOperationSpec(db),
);
res = coll.validate({checkBSONConformance: true});
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

// "Checks no errors are thrown with a valid closed bucket."
jsTestLog("Running validate on a version 2 bucket with everything correct, checking that no warnings are found.");
setUpCollection(lotsOfData);
coll = db.getCollection(collName);
res = coll.validate({checkBSONConformance: true});
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// "Checks no errors are thrown with a valid closed bucket with skipped data fields."
jsTestLog(
    "Running validate on a correct version 2 bucket with skipped data fields, checking that no warnings are found.",
);
setUpCollection(skipFieldData);
coll = db.getCollection(collName);
res = coll.validate({checkBSONConformance: true});
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, whereas before only a warning would be thrown.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
