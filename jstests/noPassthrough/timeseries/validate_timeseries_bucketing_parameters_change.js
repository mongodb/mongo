/**
 * Tests that the validate command checks data consistencies of the
 * timeseriesBucketingParametersChanged and returns errors and warnings properly.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const timeseriesBucketingParametersChangedInputValueName =
    "timeseriesBucketingParametersChangedInputValue";
let testCount = 0;
const collNamePrefix = jsTestName();
const bucketNamePrefix = "system.buckets." + jsTestName();
let collName = collNamePrefix + testCount;
let bucketName = bucketNamePrefix + testCount;
let coll = null;
let bucketsColl = null;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

// Create this timestamp to specifically fail when changing bucket size from seconds to hours
// (granularity modification) and 4 minutes to 24 hours (bucketMaxSpanSeconds and
// bucketRoundingSeconds).
const timestamp = ISODate("2024-04-01T13:25:25.000Z");

function validateTimeseriesBucketingParametersChangeFail(testCount,
                                                         originalBucketingParameterConfig,
                                                         updatedBucketingParameterConfig,
                                                         roundedDownTimestamp) {
    testCount *= 2;
    collName = collNamePrefix + testCount;
    bucketName = bucketNamePrefix + testCount;
    db.getCollection(collName).drop();
    const originalTimeSeriesParams = {
        timeField: "timestamp",
        metaField: "metadata",
        ...originalBucketingParameterConfig
    };

    jsTestLog("Create new timeseries collection.");
    assert.commandWorked(db.createCollection(collName, {timeseries: originalTimeSeriesParams}));
    coll = db.getCollection(collName);
    bucketsColl = db.getCollection(bucketName);
    // You only need to insert one doc because this test checks the minimum value of each bucket
    // and compares them to the bucket's control.min.<time field>
    coll.insert(
        {"metadata": {"sensorId": 1, "type": "temperature"}, "timestamp": timestamp, "temp": 0});

    let bucketDoc = bucketsColl.find().toArray()[0];
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        TimeseriesTest.decompressBucket(bucketDoc);
    }

    // Check that the minimum bucket timestamp is the rounded-down timestamp.
    assert.eq(roundedDownTimestamp, bucketDoc["control"]["min"]["timestamp"]);

    jsTestLog(
        "Running the validate command with a timeseries collection where we have the timeseriesBucketingParametersChanged set to False, with the bucketing parameters being changed.");
    // Set the timeseriesBucketingParametersChanged to always return false.
    assert.commandWorked(db.adminCommand({
        'configureFailPoint': timeseriesBucketingParametersChangedInputValueName,
        'mode': 'alwaysOn',
        data: {value: false}
    }));

    // This collMod should lead to timeseriesBucketingParametersChanged to True because the original
    // bucketing parameters we set for our collections was different from the updated bucketing
    // parameters.
    db.runCommand({collMod: collName, timeseries: updatedBucketingParameterConfig});

    bucketDoc = bucketsColl.find().toArray()[0];
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        TimeseriesTest.decompressBucket(bucketDoc);
    }
    // Check that the minimum bucket timestamp is the rounded-down timestamp.
    assert.eq(roundedDownTimestamp, bucketDoc["control"]["min"]["timestamp"]);

    // We have to run full validation so we decompress BSON.
    let res = bucketsColl.validate({full: true});
    assert(!res.valid, tojson(res));
    assert.eq(res.errors.length, 1);
    assert.contains(
        "A time series bucketing parameter was changed in this collection but timeseriesBucketingParametersChanged is not true. For more info, see logs with log id 9175400.",
        res.errors);
    assert.commandWorked(db.adminCommand(
        {'configureFailPoint': timeseriesBucketingParametersChangedInputValueName, 'mode': 'off'}));

    testCount += 1;
    collName = collNamePrefix + testCount;
    bucketName = bucketNamePrefix + testCount;
    db.getCollection(collName).drop();

    jsTestLog("Create new timeseries collection.");
    assert.commandWorked(db.createCollection(collName, {timeseries: originalTimeSeriesParams}));
    coll = db.getCollection(collName);
    bucketsColl = db.getCollection(bucketName);
    coll.insert(
        {"metadata": {"sensorId": 1, "type": "temperature"}, "timestamp": timestamp, "temp": 0});

    bucketDoc = bucketsColl.find().toArray()[0];
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        TimeseriesTest.decompressBucket(bucketDoc);
    }

    // Check that the minimum bucket timestamp is the rounded-down timestamp.
    assert.eq(roundedDownTimestamp, bucketDoc["control"]["min"]["timestamp"]);

    res = bucketsColl.validate({full: true});
    assert(res.valid, tojson(res));
    assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");

    jsTestLog(
        "Running the validate command with a timeseries collection where we have the timeseriesBucketingParametersChanged set to True, with the bucketing parameters NOT being changed.");
    assert.commandWorked(db.adminCommand({
        configureFailPoint: timeseriesBucketingParametersChangedInputValueName,
        mode: 'alwaysOn',
        data: {value: true}
    }));

    // This collMod shouldn't lead to timeseriesBucketingParametersChanged set to True because
    // we aren't changing the bucketing parameter config.
    db.runCommand({collMod: collName, timeseries: originalBucketingParameterConfig});

    bucketDoc = bucketsColl.find().toArray()[0];
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        TimeseriesTest.decompressBucket(bucketDoc);
    }

    // Check that the minimum bucket timestamp is the rounded-down timestamp.
    assert.eq(roundedDownTimestamp, bucketDoc["control"]["min"]["timestamp"]);

    res = bucketsColl.validate({full: true});
    assert(res.valid, tojson(res));
    assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");
    assert.commandWorked(db.adminCommand(
        {'configureFailPoint': timeseriesBucketingParametersChangedInputValueName, 'mode': 'off'}));

    jsTestLog(
        "Running the validate command with a timeseries collection where we have the timeseriesBucketingParametersChanged set to True, with the bucketing parameters being changed.");
    assert.commandWorked(db.adminCommand({
        configureFailPoint: timeseriesBucketingParametersChangedInputValueName,
        mode: 'alwaysOn',
        data: {value: true}
    }));
    db.runCommand({collMod: collName, timeseries: updatedBucketingParameterConfig});

    bucketDoc = bucketsColl.find().toArray()[0];
    if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        TimeseriesTest.decompressBucket(bucketDoc);
    }

    // Check that the minimum bucket timestamp is the rounded-down timestamp.
    assert.eq(roundedDownTimestamp, bucketDoc["control"]["min"]["timestamp"]);

    res = bucketsColl.validate({full: true});
    assert(res.valid, tojson(res));
    assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");
    assert.commandWorked(db.adminCommand(
        {'configureFailPoint': timeseriesBucketingParametersChangedInputValueName, 'mode': 'off'}));
}

// The timestamp is rounded-down to: 2024-04-01T00:00:00.000+00:00
// After the granularity change, the timestamp will be in bucket: 2024-04-01T13:25:00.000+00:00
jsTestLog(
    "Testing timeseriesBucketingParametersChange for changes to the granularity bucketing parameter.");
validateTimeseriesBucketingParametersChangeFail(
    0, {granularity: "seconds"}, {granularity: "hours"}, ISODate("2024-04-01T13:25:00.000Z"));

// The timestamp is rounded-down to: 2024-04-01T00:00:00.000+00:00
// After the bucketMaxSpanSeconds/bucketRoundingSeconds change, the timestamp will be in bucket:
// 2024-04-01T13:24:00.000+00:00
jsTestLog(
    "Testing timeseriesBucketingParametersChange for changes to the bucketRoundingSeconds and bucketMaxSpanSeconds bucketing parameters.");
validateTimeseriesBucketingParametersChangeFail(
    1,
    {bucketMaxSpanSeconds: 240, bucketRoundingSeconds: 240},  // 4 minutes
    {bucketMaxSpanSeconds: 86400, bucketRoundingSeconds: 86400},
    ISODate("2024-04-01T13:24:00.000Z"));  // 24 hours

MongoRunner.stopMongod(conn, null, {skipValidation: true});
