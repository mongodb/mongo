/**
 * Tests validating a time-series collection with mixed schema buckets when the mixed-schema flag
 * in the top-level catalog metadata, but not in the collection options (WiredTiger config string).
 *
 * This replicates the scenario where a time series collection has been created in MongoDB <5.2,
 * (i.e. before SERVER-60565) then later upgraded to MongoDB <6.0.17 (i.e. before SERVER-91195),
 * then all the way up to the current version (see SERVER-98399 for more details).
 *
 * As having the mixed-schema flag set only in the top-level catalog metadata is a precarious state,
 * as its value may be lost, direct manipulation of mixed-schema buckets is prevented, and
 * validation will flag the collection as needing manual intervention for SERVER-91194.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB(jsTestName());

const collName = "ts";

// Create a time-series collection containing a mixed-schema bucket
assert.commandWorked(testDB.runCommand({drop: collName}));
assert.commandWorked(
    testDB.createCollection(collName, {timeseries: {timeField: 't', metaField: 'm'}}));
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

assert.commandWorked(
    testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));
assert.commandWorked(
    getTimeseriesCollForRawOps(testDB, coll).insertOne(bucket, getRawOperationSpec(testDB)));

// Set the mixed-schema flag only set on the top-level catalog metadata field
// (md.timeseriesBucketsMayHaveMixedSchemaData), but not on the collection options
// (inside md.options.storageEngine.wiredTiger.configString).
const fpsimulateLegacyTimeseriesMixedSchemaFlag =
    configureFailPoint(conn, "simulateLegacyTimeseriesMixedSchemaFlag");
assert.commandWorked(
    testDB.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));
fpsimulateLegacyTimeseriesMixedSchemaFlag.off();

// TODO (SERVER-103429): Remove the rawData from $listCatalog.
const bucketsCatalogEntry = getTimeseriesCollForRawOps(testDB, coll)
                                .aggregate([{$listCatalog: {}}], getRawOperationSpec(testDB))
                                .toArray()[0];
const wtConfigStr = bucketsCatalogEntry.md.options.storageEngine?.wiredTiger?.configString ?? '';
assert.eq(true, bucketsCatalogEntry.md.timeseriesBucketsMayHaveMixedSchemaData);
assert(!wtConfigStr.includes("timeseriesBucketsMayHaveMixedSchemaData"));

// Validation of the collection returns the error asking for SERVER-91194 manual intervention
const res = assert.commandWorked(coll.validate());
assert(!res.valid);
assert.eq(res.warnings.length, 0);
assert.gt(res.errors.length, 0, "Validation should return at least one error.");
assert.containsPrefix(
    "Detected a time-series bucket with mixed schema data",
    res.errors,
    "Validation of mixed schema buckets when they are not allowed should return an error stating such");

// Direct insertion or update of mixed-schema buckets is forbidden
assert.commandFailedWithCode(getTimeseriesCollForRawOps(testDB, coll)
                                 .update({_id: bucket._id},
                                         {$set: {"control.max.a": "x", "data.a.0": "x"}},
                                         getRawOperationSpec(testDB)),
                             ErrorCodes.CannotInsertTimeseriesBucketsWithMixedSchema);

assert.commandWorked(getTimeseriesCollForRawOps(testDB, coll)
                         .deleteOne({_id: bucket._id}, getRawOperationSpec(testDB)));
assert.throwsWithCode(
    () => getTimeseriesCollForRawOps(testDB, coll).insertOne(bucket, getRawOperationSpec(testDB)),
    ErrorCodes.CannotInsertTimeseriesBucketsWithMixedSchema);

MongoRunner.stopMongod(conn);
