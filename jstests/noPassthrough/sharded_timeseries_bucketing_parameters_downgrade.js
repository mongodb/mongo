/**
 * Tests that bucketing parameters are disallowed after downgrading to versions where the parameters
 * are not supported.
 */
(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/catalog_shard_util.js");
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

    const isCatalogShardEnabled = CatalogShardUtil.isEnabledIgnoringFCV(st);

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

    const configDirectDb = st.configRS.getPrimary().getDB(dbName);
    const configDirectColl = configDirectDb.getCollection(collName);
    if (isCatalogShardEnabled) {
        // Verify we cannot downgrade if the config server has a timeseries collection with
        // bucketing.
        assert.commandWorked(configDirectDb.createCollection(collName, {
            timeseries: {
                timeField: timeField,
                metaField: metaField,
                bucketMaxSpanSeconds: 60,
                bucketRoundingSeconds: 60
            }
        }));
    }

    // On the latestFCV, we should not be able to use collMod with incomplete bucketing parameters.
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {bucketMaxSpanSeconds: 3600}}),
        ErrorCodes.InvalidOptions);

    // We should fail to downgrade if we have a collection with custom bucketing parameters set.
    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);

    coll = db.getCollection(collName);
    coll.drop();

    if (isCatalogShardEnabled) {
        // We should still fail to downgrade if we have a collection on the config server with
        // custom bucketing parameters set.
        assert.commandFailedWithCode(
            mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
            ErrorCodes.CannotDowngrade);

        configDirectColl.drop();
    }

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
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {bucketMaxSpanSeconds: 3600}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, timeseries: {bucketRoundingSeconds: 3600}}),
        ErrorCodes.InvalidOptions);

    // Verify the time-series options are valid.
    let collections = assert.commandWorked(db.runCommand({listCollections: 1})).cursor.firstBatch;
    let collectionEntry = collections.find(entry => entry.name === 'system.buckets.' + collName);
    assert(collectionEntry);

    assert.eq(collectionEntry.options.timeseries.granularity, "seconds");
    // Downgrading does not remove the 'bucketMaxSpanSeconds' parameter. It should correspond with
    // the "seconds" granularity.
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds, 3600);
    assert.isnull(collectionEntry.options.timeseries.bucketRoundingSeconds);
}

useBucketingParametersOnLowerFCV();

st.stop();
})();
