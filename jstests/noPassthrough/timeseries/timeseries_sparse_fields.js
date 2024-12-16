/**
 * Tests that measurements with sparse fields landing in the same bucket are correctly generated
 * when unpacking a bucket.
 *
 * @tags: [
 *     requires_timeseries,
 *     requires_fcv_80,
 * ]
 */

const dbName = "test";
const collName = jsTestName();

const conn = MongoRunner.runMongod();
const db = conn.getDB(dbName);

const timeField = "t";
db.createCollection(collName, {timeseries: {timeField: timeField}});

const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const shallowCompare = (obj1, obj2) => Object.keys(obj1).length === Object.keys(obj2).length &&
    Object.keys(obj1).every(key => obj1[key] === obj2[key]);

const measurements = [
    {a: 1},
    {a: 2, b: 2},
    {a: 3, b: 3, c: 3},
    {a: 4, b: 4, c: 4, d: 4},
    {a: 5, b: 5, c: 5, d: 5, e: 5},
    {a: 6, b: 6, c: 6, d: 6, e: 6, f: 6},
    {a: 7, b: 7, c: 7, d: 7, e: 7, f: 7, g: 7},
    {a: 8, b: 8, c: 8, d: 8, e: 8, f: 8, g: 8, h: 8},
    {a: 9, b: 9, c: 9, d: 9, e: 9, f: 9, g: 9, h: 9, i: 9},
    {a: 10, b: 10, c: 10, d: 10, e: 10, f: 10, g: 10, h: 10, i: 10, j: 10}
];

for (const measurement of measurements) {
    assert.commandWorked(
        coll.insert({[timeField]: ISODate("2024-02-20T00:00:00.000Z"), ...measurement}));
}

// All measurements land in the same bucket.
assert.eq(1, bucketsColl.find().length());

// Measurements are returned in insertion order and with the correct fields.
assert(coll.find().toArray().every((measurement, index) => {
    // Filter out the _id and time fields.
    delete measurement._id;
    delete measurement.t;
    return shallowCompare(measurement, measurements[index]);
}));

// Do the same as above but in reverse insertion order.
assert.commandWorked(bucketsColl.remove({}));

const reversedMeasurements = measurements.reverse();
for (const measurement of reversedMeasurements) {
    assert.commandWorked(
        coll.insert({[timeField]: ISODate("2024-02-20T00:00:00.000Z"), ...measurement}));
}

// All measurements land in the same bucket.
assert.eq(1, bucketsColl.find().length());

// Measurements are returned in insertion order and with the correct fields.
assert(coll.find().toArray().every((measurement, index) => {
    // Filter out the _id and time fields.
    delete measurement._id;
    delete measurement.t;
    return shallowCompare(measurement, reversedMeasurements[index]);
}));

MongoRunner.stopMongod(conn);
