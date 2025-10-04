/**
 * Tests directly inserting a time-series bucket with mixed schema.
 *
 * @tags: [
 *   # $listCatalog does not include the tenant prefix in its results.
 *   command_not_supported_in_serverless,
 *   requires_timeseries,
 * ]
 */

import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";

assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
const coll = testDB[collName];

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

assert.commandFailedWithCode(
    getTimeseriesCollForRawOps(coll).insert(bucket, kRawOperationSpec),
    ErrorCodes.CannotInsertTimeseriesBucketsWithMixedSchema,
);
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(getTimeseriesCollForRawOps(coll), kRawOperationSpec), false);
assert.commandWorked(testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(getTimeseriesCollForRawOps(coll), kRawOperationSpec), true);
assert.commandWorked(getTimeseriesCollForRawOps(coll).insert(bucket, kRawOperationSpec));
assert.commandWorked(getTimeseriesCollForRawOps(coll).deleteOne({_id: bucket._id}, kRawOperationSpec));

assert.commandWorked(testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: false}));
assert.eq(TimeseriesTest.bucketsMayHaveMixedSchemaData(getTimeseriesCollForRawOps(coll), kRawOperationSpec), false);
