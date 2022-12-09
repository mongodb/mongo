/**
 * Tests that bucketing parameters are disallowed after downgrading to versions where the parameters
 * are not supported.
 */
(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fail_point_util.js");

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'tm';
const metaField = 'mt';

const st = new ShardingTest({shards: 2});
const mongos = st.s0;

function useBucketingParametersOnLowerFCV() {
    const db = mongos.getDB(dbName);
    if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
        jsTestLog(
            "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
        return;
    }
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    let coll = db.getCollection(collName);
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            bucketMaxSpanSeconds: 60,
            bucketRoundingSeconds: 60
        }
    }));

    // We should fail to downgrade if we have a collection with custom bucketing parameters set.
    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);

    coll = db.getCollection(collName);
    coll.drop();

    // Successfully downgrade to latest FCV.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // On the latest FCV, we should not be able to create a collection with bucketing parameters.
    assert.commandFailedWithCode(db.createCollection(collName, {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            bucketMaxSpanSeconds: 60,
            bucketRoundingSeconds: 60
        }
    }),
                                 ErrorCodes.InvalidOptions);

    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

    // On the latest FCV we should not be able to use collMod with the bucketing parameters.
    assert.commandFailedWithCode(db.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: 3600, bucketRoundingSeconds: 3600}
    }),
                                 ErrorCodes.InvalidOptions);
}

useBucketingParametersOnLowerFCV();

st.stop();
})();
