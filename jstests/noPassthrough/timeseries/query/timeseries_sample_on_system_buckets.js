/**
 * Verifies that a direct $sample stage on time-series buckets works.
 */
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

// Prepares a timeseries collection.
assert.commandWorked(
    testDB.createCollection("t", {timeseries: {timeField: "time", metaField: "meta"}}));
assert.commandWorked(
    testDB.t.insert([{time: ISODate(), meta: 1, a: 1}, {time: ISODate(), meta: 1, a: 2}]));

// Verify that a direct $sample stage on buckets works.
const kNoOfSamples = 1;
const res = getTimeseriesCollForRawOps(testDB, testDB.t)
                .aggregate([{$sample: {size: kNoOfSamples}}], getRawOperationSpec(testDB))
                .toArray();
assert.eq(res.length, kNoOfSamples);

MongoRunner.stopMongod(conn);