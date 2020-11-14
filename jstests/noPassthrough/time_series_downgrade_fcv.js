/**
 * If a user attempts to downgrade the server while there are time-series collections present, the
 * downgrade request should be rejected.
 * @tags: [
 *     requires_fcv_49,
 *     requires_find_command,
 * ]
 */
(function() {
"use strict";

load('jstests/core/time_series/libs/time_series.js');

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const testDB = conn.getDB(jsTestName());
const coll = testDB.getCollection('t');

const timeFieldName = 'time';
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

// Downgrade the primary using the setFeatureCompatibilityVersion command.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// This confirms that the downgrade command will complete successfully after the time-series
// collection has been removed.
assert(coll.drop());
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

MongoRunner.stopMongod(conn);
})();
