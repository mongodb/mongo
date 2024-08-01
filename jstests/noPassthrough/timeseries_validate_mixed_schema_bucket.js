/**
 * Tests validating a time-series collection with mixed schema buckets.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const conn = MongoRunner.runMongod();
const testDB = conn.getDB(jsTestName());

const collName = "ts";

testDB.createCollection(collName, {timeseries: {timeField: "timestamp", metaField: "metadata"}});

configureFailPoint(conn, "allowSetTimeseriesBucketsMayHaveMixedSchemaDataFalse");

assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
const coll = testDB[collName];
const bucketsColl = testDB["system.buckets." + collName];

const timeseriesBucketsMayHaveMixedSchemaData = function() {
    return bucketsColl.aggregate([{$listCatalog: {}}])
        .toArray()[0]
        .md.timeseriesBucketsMayHaveMixedSchemaData;
};

const bucket = {
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: NumberInt(1),
        min: {
            _id: ObjectId("65a6eba7e6d2e848e08c3750"),
            t: ISODate("2024-01-16T20:48:00Z"),
            a: 1,
        },
        max: {
            _id: ObjectId("65a6eba7e6d2e848e08c3751"),
            t: ISODate("2024-01-16T20:48:39.448Z"),
            a: "a",
        },
    },
    meta: 0,
    data: {
        _id: {
            0: ObjectId("65a6eba7e6d2e848e08c3750"),
            1: ObjectId("65a6eba7e6d2e848e08c3751"),
        },
        t: {
            0: ISODate("2024-01-16T20:48:39.448Z"),
            1: ISODate("2024-01-16T20:48:39.448Z"),
        },
        a: {
            0: "a",
            1: 1,
        },
    },
};

assert.commandWorked(
    testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));
assert.eq(timeseriesBucketsMayHaveMixedSchemaData(), true);

// There should be no reason to have validation errors in the empty collection.
let res = assert.commandWorked(coll.validate());
assert(res.valid);
assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");

// Insert normal bucket——we expect no errors nor warnings.
assert.commandWorked(coll.insert({t: ISODate()}));
res = assert.commandWorked(coll.validate());
assert(res.valid);
assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");
assert.eq(res.warnings.length, 0, "Validation warnings detected when there should be none.");

assert.commandWorked(bucketsColl.insert(bucket));
// Even though a mixed schema bucket has been inserted, since the mixed schema data is allowed there
// should be no errors.
res = assert.commandWorked(coll.validate());
assert(res.valid);
assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");
// There should be one warning, though, indicating the mixed schema data.
assert.eq(res.warnings.length, 1);
assert.containsPrefix("Detected a time-series bucket with mixed schema data", res.warnings);

assert.commandWorked(
    testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: false}));

// Now that the allow mixed schema flag is false, an error should be returned after validating.
res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.eq(res.warnings.length, 0);
assert.gt(res.errors.length, 0, "Validation should return at least one error.");
assert.containsPrefix(
    "Detected a time-series bucket with mixed schema data",
    res.errors,
    "Validation of mixed schema buckets when they are not allowed should return an error stating such");

MongoRunner.stopMongod(conn, null, {skipValidation: true});
})();
