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
const collNamePrefix = "validate_timeseries_bucketing_parameters_change";
const bucketNamePrefix = "system.buckets.validate_timeseries_bucketing_parameters_change";
let collName = collNamePrefix + testCount;
let bucketName = bucketNamePrefix + testCount;
let coll = null;
let bucket = null;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

function validateTimeseriesBucketingParametersChangeFail(
    testCount, originalBucketingParameterConfig, updatedBucketingParameterConfig) {
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
    bucket = db.getCollection(bucketName);
    TimeseriesTest.insertManyDocs(coll);

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

    // We have to run full validation so we decompress BSON.
    let res = bucket.validate({full: true});
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
    bucket = db.getCollection(bucketName);
    TimeseriesTest.insertManyDocs(coll);

    // We should have a the validate results be valid.
    res = bucket.validate({full: true});
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
    res = bucket.validate({full: true});
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
    res = bucket.validate({full: true});
    assert(res.valid, tojson(res));
    assert.eq(res.errors.length, 0, "Validation errors detected when there should be none.");
    assert.commandWorked(db.adminCommand(
        {'configureFailPoint': timeseriesBucketingParametersChangedInputValueName, 'mode': 'off'}));
}

jsTestLog(
    "Testing timeseriesBucketingParametersChange for changes to the granularity bucketing parameter.");
validateTimeseriesBucketingParametersChangeFail(
    0, {granularity: "seconds"}, {granularity: "hours"});

jsTestLog(
    "Testing timeseriesBucketingParametersChange for changes to the bucketRoundingSeconds and bucketMaxSpanSeconds bucketing parameters.");
validateTimeseriesBucketingParametersChangeFail(
    1,
    {bucketMaxSpanSeconds: 240, bucketRoundingSeconds: 240},
    {bucketMaxSpanSeconds: 86400, bucketRoundingSeconds: 86400});

MongoRunner.stopMongod(conn, null, {skipValidation: true});