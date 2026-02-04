/**
 * Tests that validate will detect a compressed bucket with time out-of-order.
 */
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const collName = "ts";
const testDB = conn.getDB(dbName);
const tsColl = testDB[collName];

const timeField = "t";
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField}}));

const oidStr = "630ea4802093f9983fc394dc";
// Compressed bucket with the compressed time field out-of-order.
const tsRecord = {
    "_id": ObjectId(oidStr),
    "control": {
        "version": NumberInt(2),
        "min": {
            "_id": ObjectId("630fabf7c388456f8aea4f2d"),
            [timeField]: ISODate("2022-08-31T00:00:00.000Z"),
            "a": 0,
        },
        "max": {
            "_id": ObjectId("630fabf7c388456f8aea4f2f"),
            [timeField]: ISODate("2022-08-31T00:00:01.000Z"),
            "a": 1,
        },
        "count": 2,
    },
    "data": {
        [timeField]: BinData(7, "CQDolzLxggEAAID+fAAAAAAAAAA="),
        "_id": BinData(7, "BwBjD6v3w4hFb4rqTy2ATgAAAAAAAAAA"),
        "a": BinData(7, "EAAAAAAAgC4AAAAAAAAAAA=="),
    },
};
assert.commandWorked(getTimeseriesCollForRawOps(testDB, tsColl).insertOne(tsRecord, getRawOperationSpec(testDB)));

let res = assert.commandWorked(tsColl.validate({checkBSONConformance: true}));
assert(!res.valid);
assert.eq(res.errors.length, 1);

TimeseriesTest.checkForDocumentValidationFailureLog(getTimeseriesCollForRawOps(testDB, tsColl), tsRecord);

MongoRunner.stopMongod(conn, null, {skipValidation: true});
