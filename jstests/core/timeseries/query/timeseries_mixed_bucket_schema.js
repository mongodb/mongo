/**
 * This test ensures that a timeseries bucket with a mixed schema (i.e. before we separated buckets by
 * schema) produces correct results.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 *
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const testDB = db;

const collName = jsTestName();

const coll = testDB[collName];
coll.drop();
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));

// BSON values are ordered by (normalized type) and then value.
// Since the array type is greater than numeric types [2,3] will be the max and 4 will be the min.
const bucket = {
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: NumberInt(1),
        min: {
            _id: ObjectId("65a6eba7e6d2e848e08c3750"),
            t: ISODate("2024-01-16T20:48:00Z"),
            a: 4,
        },
        max: {
            _id: ObjectId("65a6eba7e6d2e848e08c3751"),
            t: ISODate("2024-01-16T20:48:39.448Z"),
            a: [2, 3],
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
            0: [2, 3],
            1: 4,
        },
    },
};

// Manually set the flag.
assert.commandWorked(testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(coll), true);

// Insert mixed data into the collection.
assert.commandWorked(getTimeseriesCollForRawOps(testDB, coll).insertOne(bucket, getRawOperationSpec(testDB)));

/**
 * If mixed schema rewrites do not work, this will get rewritten to a predicate on control.min under
 * the assumptions that values are separated by types.
 * However, the bucket that we've inserted contains a mix of ints and arrays of ints with the all
 * the ints stricly greater than 3 and the arrays containing values less than 3. Since our match
 * semantics allow us to match the values inside the array, this will only match if the rewrite to
 * control.min does not occur and either the mixed schem rewrite or some other rewrite does occur.
 */
const results = coll.aggregate([{$match: {a: {$lte: 3}}}]).toArray();
assert.eq(results.length, 1);
// Ensure the result is as expected.
assert.eq(results[0]["a"], [2, 3]);
