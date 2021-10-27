/**
 * Tests that the upgrade process does not crash when time-series collections are created during the
 * FCV upgrade process.
 *
 * TODO SERVER-60912: Remove this test once kLastLTS is 6.0.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fail_point_util.js");

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

// Set FCV to 5.0.
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Set fail point to fail the upgrade.
let upgradeFailPoint = configureFailPoint(conn, "failUpgrading");

// Start the upgrade, this will fail and we'll be in a semi-upgraded state.
assert.commandFailed(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

upgradeFailPoint.wait();
upgradeFailPoint.off();

// Create a time-series collection while we're semi-upgraded. This time-series collection will have
// the 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag set to false.
const collName = jsTestName();
const coll = db.getCollection(collName);

const timeFieldName = "tm";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

// Re-try the upgrade after creating a time-series collection in a semi-upgraded state. This should
// not crash.
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

MongoRunner.stopMongod(conn);
}());
