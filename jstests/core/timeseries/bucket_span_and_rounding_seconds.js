/**
 * Tests timeseries collection creation with bucketRoundingSeconds and bucketMaxSpanSeconds
 * parameters and checks that we correctly set their value (failing when parameters are
 * not added correctly or are missing).
 *
 * @tags: [
 *   # "Overriding safe failed response for :: create"
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = 'time';
const coll = testDB.t;
const bucketRoundingSecondsTime = 4000;
const bucketMaxSpanSecondsTime = 4000;
const granularitySeconds = "seconds";
const granularityMinutes = "minutes";
const granularityHours = "hours";
const bucketInvalidOptionsError = ErrorCodes.InvalidOptions;

const granularityTimeOptionsArr = [granularitySeconds, granularityMinutes, granularityHours];

const verifyCreateCommandFails = function(secondsOptions = {}, errorCode) {
    coll.drop();
    const fullTimeseriesOptions = Object.merge({timeField: timeFieldName}, secondsOptions);

    if (errorCode) {
        assert.commandFailedWithCode(
            testDB.createCollection(coll.getName(), {timeseries: fullTimeseriesOptions}),
            errorCode);
    } else {
        assert.commandFailed(
            testDB.createCollection(coll.getName(), {timeseries: fullTimeseriesOptions}));
    }

    const collections =
        assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;

    assert.isnull(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
    assert.isnull(collections.find(entry => entry.name === coll.getName()));
};

(function createTimeseriesCollectionWithBucketSecondsOptions() {
    jsTestLog("Create timeseries collection with bucketRoundingSeconds and bucketMaxSpanSeconds.");
    // Create a timeseries collection with bucketRoundingSeconds, bucketMaxSpanSeconds and
    // custom parameters. ListCollection should show view and bucket collection with the added
    // properties.
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
            bucketRoundingSeconds: bucketRoundingSecondsTime,
            bucketMaxSpanSeconds: bucketMaxSpanSecondsTime
        }
    }));

    let collections =
        assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;

    let collectionEntry =
        collections.find(entry => entry.name === 'system.buckets.' + coll.getName());
    assert(collectionEntry);
    assert.eq(collectionEntry.options.timeseries.bucketRoundingSeconds, bucketRoundingSecondsTime);
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds, bucketMaxSpanSecondsTime);

    collectionEntry = collections.find(entry => entry.name === coll.getName());
    assert(collectionEntry);
    assert.eq(collectionEntry.options.timeseries.bucketRoundingSeconds, bucketRoundingSecondsTime);
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds, bucketMaxSpanSecondsTime);

    // Verify the create command succeeds with bucketRoundingSeconds, bucketMaxSpanSeconds set as
    // their default granularity values.
    for (const granularityTime of granularityTimeOptionsArr) {
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName(), {
            timeseries: {
                timeField: timeFieldName,
                granularity: granularityTime,
                bucketRoundingSeconds:
                    TimeseriesTest.getBucketRoundingSecondsFromGranularity(granularityTime),
                bucketMaxSpanSeconds:
                    TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularityTime)
            }
        }));
        collections =
            assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;

        collectionEntry =
            collections.find(entry => entry.name === 'system.buckets.' + coll.getName());
        assert(collectionEntry);
        assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
        assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
                  TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularityTime));

        collectionEntry = collections.find(entry => entry.name === coll.getName());
        assert(collectionEntry);
        assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
        assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
                  TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularityTime));
    }

    // Verify the create command succeeds without setting bucketRoundingSeconds and
    // bucketMaxSpanSeconds. This should set their default granularity values.
    for (const granularityTime of granularityTimeOptionsArr) {
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName(), {
            timeseries: {
                timeField: timeFieldName,
                granularity: granularityTime,
            }
        }));
        collections =
            assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;

        collectionEntry =
            collections.find(entry => entry.name === 'system.buckets.' + coll.getName());
        assert(collectionEntry);
        assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
        assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
                  TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularityTime));

        collectionEntry = collections.find(entry => entry.name === coll.getName());
        assert(collectionEntry);
        assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
        assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
                  TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularityTime));
    }

    // Verify the create command succeeds without setting any field other than timeField, this
    // should set granularity as seconds and bucketRoundingSeconds and bucketMaxSpanSeconds with
    // their default granularity values.
    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
        }
    }));
    collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;

    collectionEntry = collections.find(entry => entry.name === 'system.buckets.' + coll.getName());
    assert(collectionEntry);
    assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
              TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularitySeconds));

    collectionEntry = collections.find(entry => entry.name === coll.getName());
    assert(collectionEntry);
    assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds,
              TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(granularitySeconds));
})();

(function createTimeseriesCollectionWithInvalidOptions() {
    jsTestLog("Create timeseries collection with missing or extra arguments.");

    // Verify the create command fails when the 'bucketRoundingSeconds' option is set but not the
    // 'bucketMaxSpanSeconds' option.
    verifyCreateCommandFails({bucketRoundingSeconds: bucketRoundingSecondsTime},
                             bucketInvalidOptionsError);

    // Verify the create command fails when the 'bucketMaxSpanSeconds' option is set but not the
    // 'bucketRoundingSeconds' option.
    verifyCreateCommandFails({bucketMaxSpanSeconds: bucketMaxSpanSecondsTime},
                             bucketInvalidOptionsError);

    // Verify the create command fails when the 'bucketMaxSpanSeconds' option is set but not the
    // 'bucketRoundingSeconds' option (even if set to granularity default seconds value).
    verifyCreateCommandFails({bucketMaxSpanSeconds: 3600}, bucketInvalidOptionsError);

    // Verify the create command fails when bucketRoundingSeconds is different than
    // bucketMaxSpanSeconds.
    verifyCreateCommandFails({bucketRoundingSeconds: 100, bucketMaxSpanSeconds: 50},
                             bucketInvalidOptionsError);

    // Verify the create command fails when bucketRoundingSeconds or bucketMaxSpanSeconds is a
    // negative value.
    verifyCreateCommandFails({bucketRoundingSeconds: -1, bucketMaxSpanSeconds: -1});

    // Verify the create command fails when granularity is set as minutes alongside
    // bucketRoundingSeconds and bucketMaxSpanSeconds and they are not the default granularity
    // values.
    verifyCreateCommandFails({
        granularity: granularityMinutes,
        bucketRoundingSeconds: bucketRoundingSecondsTime,
        bucketMaxSpanSeconds: bucketMaxSpanSecondsTime
    },
                             bucketInvalidOptionsError);

    // Verify the create command fails when granularity is set as hours alongside
    // bucketRoundingSeconds and bucketMaxSpanSeconds and they are not the default granularity
    // values.
    verifyCreateCommandFails({
        granularity: granularityHours,
        bucketRoundingSeconds: bucketRoundingSecondsTime,
        bucketMaxSpanSeconds: bucketMaxSpanSecondsTime
    },
                             bucketInvalidOptionsError);
})();
})();
