/**
 * Tests that the cluster cannot be downgraded when there are secondary TTL indexes with partial
 * filters on time-series present.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/feature_flag_util.js");  // For isEnabled.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesScalabilityImprovements")) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

const collName = "timeseries_ttl_index_downgrade";
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const timeFieldName = "tm";
const metaFieldName = "mm";
const timeSpec = {
    [timeFieldName]: 1
};

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

function checkIndexForDowngrade(isCompatible) {
    if (!isCompatible) {
        assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                     ErrorCodes.CannotDowngrade);
        assert.commandWorked(coll.dropIndexes("*"));
    }

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}

// Verify that downgrading succeeds on a time-series collection without any indexes.
checkIndexForDowngrade(true);

// Verify that downgrading succeeds on a time-series collection with a partial index.
const options = {
    name: "partialIndexOnMeta",
    partialFilterExpression: {[metaFieldName]: {$gt: 5}}
};
assert.commandWorked(coll.createIndex(timeSpec, options));
checkIndexForDowngrade(true);

// Verify that downgrading succeeds on a time-series collection created with expireAfterSeconds
// value.
coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, metaField: metaFieldName}, expireAfterSeconds: 3600}));
checkIndexForDowngrade(true);

// Verify that downgrading fails on a time-series collection with a partial, TTL index.
assert.commandWorked(coll.createIndex(timeSpec, Object.merge(options, {expireAfterSeconds: 400})));
checkIndexForDowngrade(false);

MongoRunner.stopMongod(conn);
}());
