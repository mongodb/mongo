/**
 * Tests directly updating a time-series bucket to contain mixed schema.
 *
 * @tags: [
 *   # $listCatalog does not include the tenant prefix in its results.
 *   command_not_supported_in_serverless,
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";
load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

TestData.skipEnforceTimeseriesBucketsAreAlwaysCompressedOnValidate = true;

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
const coll = testDB[collName];
const bucketsColl = testDB["system.buckets." + collName];

const bucket = {
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: NumberInt(1),
        min: {
            _id: ObjectId("65a6eba7e6d2e848e08c3750"),
            t: ISODate("2024-01-16T20:48:00Z"),
            a: 0,
        },
        max: {
            _id: ObjectId("65a6eba7e6d2e848e08c3751"),
            t: ISODate("2024-01-16T20:48:39.448Z"),
            a: 1,
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
            0: 0,
            1: 1,
        },
    }
};

const update = function() {
    return bucketsColl.update({_id: bucket._id},
                              {$set: {"control.min.a": 1, "control.max.a": "a", "data.a.0": "a"}});
};

assert.commandWorked(bucketsColl.insert(bucket));
assert.commandFailedWithCode(update(), ErrorCodes.CannotInsertTimeseriesBucketsWithMixedSchema);
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(bucketsColl), false);
assert.commandWorked(
    testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(bucketsColl), true);
assert.commandWorked(update());
assert.commandWorked(bucketsColl.deleteOne({_id: bucket._id}));
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(bucketsColl), true);

// We perform an fsync to move the stable checkpoint up to include the deletion of the mixed schema
// document, so that background validation does not see an inconsistent state when checking the
// latest catalog value for the mixed schema flag (i.e, prevent it from detecting the presence of a
// mixed schema bucket with the mixed schema flag set to false).
assert.commandWorked(testDB.adminCommand({fsync: 1}));

assert.commandWorked(
    testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: false}));
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(bucketsColl), false);
})();
