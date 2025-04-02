/**
 * Tests that schema validation is enabled on the bucket collection.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const coll = db.getCollection(jsTestName());
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
const oid = ObjectId("65F9971847423af45aeafc67");
const timestamp = ISODate("2024-03-19T13:46:00Z");
assert.commandWorked(bucketsColl.insert({
    _id: oid,
    control: {
        version: TimeseriesTest.BucketVersion.kCompressedSorted,
        min: {time: timestamp, "_id": oid, "a": 1},
        max: {time: timestamp, "_id": oid, "a": 1},
        count: 1
    },
    data: {
        "time": BinData(7, "CQDANfZWjgEAAAA="),
        "_id": BinData(7, "BwBl+ZcYR0I69Frq/GcA"),
        "a": BinData(7, "AQAAAAAAAADwPwA="),
    }
}));

assert.commandFailedWithCode(bucketsColl.insert({
    control: {version: 'not a number', min: {time: ISODate()}, max: {time: ISODate()}},
    data: {}
}),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(bucketsColl.insert({
    control: {
        version: TimeseriesTest.BucketVersion.kUncompressed,
        min: {time: 'not a date'},
        max: {time: ISODate()}
    },
    data: {}
}),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(bucketsColl.insert({
    control: {
        version: TimeseriesTest.BucketVersion.kUncompressed,
        min: {time: ISODate()},
        max: {time: 'not a date'}
    },
    data: {}
}),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(bucketsColl.insert({
    control: {
        version: TimeseriesTest.BucketVersion.kUncompressed,
        min: {time: ISODate()},
        max: {time: ISODate()}
    },
    data: 'not an object'
}),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(bucketsColl.insert({invalid_bucket_field: 1}),
                             ErrorCodes.DocumentValidationFailure);
assert.commandWorked(db.runCommand({drop: coll.getName(), writeConcern: {w: "majority"}}));
