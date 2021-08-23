/**
 * Tests that the cluster cannot be downgraded when there are secondary indexes on time-series
 * measurements present. Additionally, this verifies that only indexes that are incompatible for
 * downgrade have the "originalSpec" field present on the buckets index definition.
 *
 * TODO SERVER-60912: Remove this test once kLastLTS is 6.0.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

const collName = "timeseries_measurement_indexes_downgrade";
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const timeFieldName = "tm";
const metaFieldName = "mm";

assert.commandWorked(db.createCollection("regular"));
assert.commandWorked(db.createCollection("system.buckets.abc"));

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

function checkIndexForDowngrade(withFCV, isCompatible, createdOnBucketsCollection) {
    const index = bucketsColl.getIndexes()[0];

    if (isCompatible) {
        // All time-series indexes are downgrade compatible to the last continuous FCV as of v5.3.
        if (withFCV != lastContinuousFCV) {
            assert(!index.hasOwnProperty("originalSpec"));
        }
    } else {
        if (createdOnBucketsCollection) {
            // Indexes created directly on the buckets collection do not have the original user
            // index definition.
            assert(!index.hasOwnProperty("originalSpec"));
        } else {
            assert(index.hasOwnProperty("originalSpec"));
        }

        assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: withFCV}),
                                     ErrorCodes.CannotDowngrade);
        assert.commandWorked(coll.dropIndexes("*"));
    }

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: withFCV}));
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    assert.commandWorked(coll.dropIndexes("*"));
}

assert.commandWorked(coll.createIndex({[timeFieldName]: 1}));
checkIndexForDowngrade(lastLTSFCV, true, false);

assert.commandWorked(coll.createIndex({[timeFieldName]: 1}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: 1}));
checkIndexForDowngrade(lastLTSFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: 1}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: 1, a: 1}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: 1, a: 1}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(coll.createIndex({b: 1}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(coll.createIndex({b: 1}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(bucketsColl.createIndex({"control.min.c.d": 1, "control.max.c.d": 1}));
checkIndexForDowngrade(lastLTSFCV, false, true);

assert.commandWorked(bucketsColl.createIndex({"control.min.c.d": 1, "control.max.c.d": 1}));
checkIndexForDowngrade(lastContinuousFCV, true, true);

assert.commandWorked(bucketsColl.createIndex({"control.min.e": 1, "control.min.f": 1}));
checkIndexForDowngrade(lastLTSFCV, false, true);

assert.commandWorked(bucketsColl.createIndex({"control.min.e": 1, "control.min.f": 1}));
checkIndexForDowngrade(lastContinuousFCV, true, true);

assert.commandWorked(coll.createIndex({g: "2dsphere"}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(coll.createIndex({g: "2dsphere"}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: "2d"}));
checkIndexForDowngrade(lastLTSFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: "2d"}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: "2dsphere"}));
checkIndexForDowngrade(lastLTSFCV, true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: "2dsphere"}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

// Partial indexes are not supported in versions earlier than v5.2.
assert.commandWorked(
    coll.createIndex({[timeFieldName]: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(
    coll.createIndex({[timeFieldName]: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(
    coll.createIndex({[metaFieldName]: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(
    coll.createIndex({[metaFieldName]: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

assert.commandWorked(
    coll.createIndex({[metaFieldName]: 1, x: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastLTSFCV, false, false);

assert.commandWorked(
    coll.createIndex({x: 1, [metaFieldName]: 1}, {partialFilterExpression: {x: {$gt: 5}}}));
checkIndexForDowngrade(lastContinuousFCV, true, false);

MongoRunner.stopMongod(conn);
}());
