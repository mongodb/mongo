/**
 * Tests that validate will detect that a v3 bucket is not un-sorted, or in other words, that v2
 * buckets aren't wrongly promoted to v3 buckets.
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const conn = MongoRunner.runMongod();
const dbName = jsTestName();
const collName = 'ts';
const testDB = conn.getDB(dbName);
const tsColl = testDB[collName];
const bucketsColl = testDB.getCollection('system.buckets.' + collName);

const timeField = 't';
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField}}));

// Compressed bucket with the compressed time field out-of-order and version set to 3. This is
// expected for a v3 bucket, and validation should pass here.
assert.commandWorked(bucketsColl.insert({
    "_id": ObjectId("630ea4802093f9983fc394dc"),
    "control": {
        "version": TimeseriesTest.BucketVersion.kCompressedUnsorted,
        "min": {
            "_id": ObjectId("630fabf7c388456f8aea4f2d"),
            "t": ISODate("2022-08-31T00:00:00.000Z"),
            "a": 0
        },
        "max": {
            "_id": ObjectId("630fabf7c388456f8aea4f2f"),
            "t": ISODate("2022-08-31T00:00:01.000Z"),
            "a": 1
        },
        "count": 2
    },
    "data": {
        "t": BinData(7, "CQDolzLxggEAAID+fAAAAAAAAAA="),
        "_id": BinData(7, "BwBjD6v3w4hFb4rqTy2ATgAAAAAAAAAA"),
        "a": BinData(7, "EAAAAAAAgC4AAAAAAAAAAA==")
    }
}));
let res = assert.commandWorked(tsColl.validate({full: true}));
assert(res.valid);

// Compressed bucket with the compressed time field in-order and version set to 3. This should fail,
// since this bucket's measurements are in-order on time field, meaning this bucket shouldn't have
// been promoted to v3.
assert.commandWorked(bucketsColl.insert({
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: TimeseriesTest.BucketVersion.kCompressedUnsorted,
        min: {
            _id: ObjectId("65a6eba7e6d2e848e08c3750"),
            t: ISODate("2024-01-16T20:48:00Z"),
            a: 1,
        },
        max: {
            _id: ObjectId("65a6eba7e6d2e848e08c3751"),
            t: ISODate("2024-01-16T20:48:39.448Z"),
            a: 1,
        },
        count: NumberInt(2),
    },
    meta: 0,
    data: {
        t: BinData(7, "CQAYhggUjQEAAIAOAAAAAAAAAAA="),
        a: BinData(7, "AQAAAAAAAAAAAJAuAAAAAAAAAAA="),
        _id: BinData(7, "BwBlpuun5tLoSOCMN1CALgAAAAAAAAAA"),
    }
}));
res = assert.commandWorked(tsColl.validate({full: true}));
assert(!res.valid);
assert.eq(res.errors.length, 1);

MongoRunner.stopMongod(conn, null, {skipValidation: true});