/**
 * Tests that schema validation is enabled on the buckets of a time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const coll = db.getCollection(jsTestName());
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "time"}}));
const oid = ObjectId("65F9971847423af45aeafc67");
const timestamp = ISODate("2024-03-19T13:46:00Z");
assert.commandWorked(getTimeseriesCollForRawOps(coll).insert({
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
},
                                                             kRawOperationSpec));

assert.commandFailedWithCode(getTimeseriesCollForRawOps(coll).insert({
    control: {version: 'not a number', min: {time: ISODate()}, max: {time: ISODate()}},
    data: {}
},
                                                                     kRawOperationSpec),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(getTimeseriesCollForRawOps(coll).insert({
    control: {
        version: TimeseriesTest.BucketVersion.kUncompressed,
        min: {time: 'not a date'},
        max: {time: ISODate()}
    },
    data: {}
},
                                                                     kRawOperationSpec),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(getTimeseriesCollForRawOps(coll).insert({
    control: {
        version: TimeseriesTest.BucketVersion.kUncompressed,
        min: {time: ISODate()},
        max: {time: 'not a date'}
    },
    data: {}
},
                                                                     kRawOperationSpec),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(getTimeseriesCollForRawOps(coll).insert({
    control: {
        version: TimeseriesTest.BucketVersion.kUncompressed,
        min: {time: ISODate()},
        max: {time: ISODate()}
    },
    data: 'not an object'
},
                                                                     kRawOperationSpec),
                             ErrorCodes.DocumentValidationFailure);
assert.commandFailedWithCode(
    getTimeseriesCollForRawOps(coll).insert({invalid_bucket_field: 1}, kRawOperationSpec),
    ErrorCodes.DocumentValidationFailure);
assert.commandWorked(db.runCommand({drop: coll.getName(), writeConcern: {w: "majority"}}));
