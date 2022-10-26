/**
 * Tests behavior with the bucketing parameters on time-series collections when downgrading. If we
 * are using custom bucketing parameters we expect to fail the downgrade but if we use default
 * granularity values the downgrade should succeed.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/feature_flag_util.js");  // For isEnabled.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(conn)) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

const collName = "timeseries_bucketing_parameters";
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const timeFieldName = "tm";
const metaFieldName = "mm";

const resetCollection = function(extraOptions = {}) {
    coll.drop();

    const tsOpts = {timeField: timeFieldName, metaField: metaFieldName};
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: Object.merge(tsOpts, extraOptions)}));
    assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: 1}));
};

const secondsMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('seconds');
const secondsRoundingSeconds = TimeseriesTest.getBucketRoundingSecondsFromGranularity('seconds');
const minutesMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('minutes');
const minutesRoundingSeconds = TimeseriesTest.getBucketRoundingSecondsFromGranularity('minutes');
const hoursMaxSpan = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('hours');
const hoursRoundingSeconds = TimeseriesTest.getBucketRoundingSecondsFromGranularity('hours');

const getNearestGranularity = function(bucketingParams) {
    assert(bucketingParams.hasOwnProperty('bucketMaxSpanSeconds') &&
           bucketingParams.hasOwnProperty('bucketRoundingSeconds'));

    if (bucketingParams.bucketMaxSpanSeconds <= secondsMaxSpan &&
        bucketingParams.bucketRoundingSeconds <= secondsRoundingSeconds) {
        return 'seconds';
    }

    if (bucketingParams.bucketMaxSpanSeconds <= minutesMaxSpan &&
        bucketingParams.bucketRoundingSeconds <= minutesRoundingSeconds) {
        return 'minutes';
    }

    if (bucketingParams.bucketMaxSpanSeconds <= hoursMaxSpan &&
        bucketingParams.bucketRoundingSeconds <= hoursRoundingSeconds) {
        return 'hours';
    }

    return null;
};

// Checks if the downgrade command succeeds and reset the version to latestFCV.
function checkDowngradeSucceeds() {
    // Verify that downgrade succeeds.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // Check that the 'granularity' and 'bucketMaxSpanSeconds' are correctly set and that
    // 'bucketRoundingSeconds' is not set to any value.
    let collections = assert.commandWorked(db.runCommand({listCollections: 1})).cursor.firstBatch;
    let collectionEntry =
        collections.find(entry => entry.name === 'system.buckets.' + coll.getName());
    assert(collectionEntry);

    let granularity = collectionEntry.options.timeseries.granularity;
    assert(granularity);
    assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
              TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularity));

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}

// Checks that downgrade fails but tries again by using the collMod command to modify the collection
// into a downgradable state. Will drop the collection if there is no possible granularity to
// update.
function checkDowngradeFailsAndTryAgain(bucketingParams) {
    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);

    let nextGranularity = getNearestGranularity(bucketingParams);

    if (nextGranularity) {
        assert.commandWorked(
            db.runCommand({collMod: collName, timeseries: {granularity: nextGranularity}}));
    } else {
        // If the bucketMaxSpanSeconds and bucketRoundingSeconds are both greater than the values
        // corresponding to the 'hours' granularity, the only way to successfully downgrade is to
        // drop the collection.
        resetCollection();
    }

    checkDowngradeSucceeds();
}

const checkBucketCount = function(count = 1) {
    let stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    assert.eq(stats.timeseries['bucketCount'], count);
};

// 1. We expect downgrade to work seamlessly when a standard granularity is used.
{
    resetCollection();

    // When we create collections with no granularity specified, we should default to 'seconds'
    // meaning we should be able to downgrade successfully.
    checkDowngradeSucceeds();

    // If we explicitly set the granularity of a collection we expect to succesfully downgrade.
    resetCollection({granularity: 'seconds'});
    checkDowngradeSucceeds();

    // We expect to successfully downgrade with different granularity values.
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: "seconds"}}));
    checkDowngradeSucceeds();
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: "minutes"}}));
    checkDowngradeSucceeds();
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: {granularity: "hours"}}));
    checkDowngradeSucceeds();
}

// 2. We expect to successfully downgrade if the 'bucketMaxSpanSeconds' and 'bucketRoundingSeconds'
// correspond to a granularity.
{
    resetCollection({granularity: 'seconds', bucketMaxSpanSeconds: secondsMaxSpan});
    checkDowngradeSucceeds();

    resetCollection({
        granularity: 'seconds',
        bucketMaxSpanSeconds: secondsMaxSpan,
        bucketRoundingSeconds: secondsRoundingSeconds
    });
    checkDowngradeSucceeds();

    resetCollection({granularity: 'minutes', bucketMaxSpanSeconds: minutesMaxSpan});
    checkDowngradeSucceeds();

    resetCollection({granularity: 'hours', bucketMaxSpanSeconds: hoursMaxSpan});
    checkDowngradeSucceeds();
}

// 3. When we set values for 'bucketMaxSpanSeconds' and 'bucketRoundingSeconds' we expect downgrade
// to fail. Changing the collection's granularity to the next possible granularity should allow
// downgrade to succeed.
{
    // Use custom bucketing parameters (less than the 'seconds' granularity).
    let bucketingParams = {
        bucketMaxSpanSeconds: secondsRoundingSeconds,
        bucketRoundingSeconds: secondsRoundingSeconds
    };
    resetCollection(bucketingParams);

    // Insert a few measurements to create a total of 3 buckets.
    assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: 2}));
    assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: 3}));
    checkBucketCount(3);

    // Expect downgrade to fail but when the granularity is changed to 'seconds' we should
    // successfully downgrade.
    checkDowngradeFailsAndTryAgain(bucketingParams);

    // Use custom bucketing parameters (less than the 'minutes' granularity).
    bucketingParams = {bucketMaxSpanSeconds: secondsMaxSpan, bucketRoundingSeconds: secondsMaxSpan};
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: bucketingParams}));

    // Expect downgrade to fail but when the granularity is changed to 'minutes' we should
    // successfully downgrade.
    checkDowngradeFailsAndTryAgain(bucketingParams);

    // Use custom bucketing parameters (less than the 'hours' granularity).
    bucketingParams = {bucketMaxSpanSeconds: minutesMaxSpan, bucketRoundingSeconds: minutesMaxSpan};
    assert.commandWorked(db.runCommand({collMod: collName, timeseries: bucketingParams}));

    // Expect downgrade to fail but when the granularity is changed to 'hours' we should
    // successfully downgrade.
    checkDowngradeFailsAndTryAgain(bucketingParams);

    // Make sure the collection did not get dropped in the process to successfully downgrade by
    // checking the number of buckets in the collection.
    checkBucketCount(3);
}

// 4. In cases where the bucketing parameters are higher than the possible granularities, the only
// way to downgrade is to drop the collection.
{
    let bucketingParams = {bucketMaxSpanSeconds: hoursMaxSpan, bucketRoundingSeconds: hoursMaxSpan};
    resetCollection(bucketingParams);

    // Insert a few measurements to create a total of 3 buckets.
    assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: 2}));
    assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: 3}));
    checkBucketCount(3);

    // Expect the downgrade to fail and drops the collection for the downgrade to succeed.
    checkDowngradeFailsAndTryAgain(bucketingParams);

    // Verify the original collection had to be dropped in order to downgrade.
    checkBucketCount(1);
}

MongoRunner.stopMongod(conn);
}());
