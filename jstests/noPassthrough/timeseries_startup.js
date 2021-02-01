/**
 * Tests that the server can startup with time-series collections present.
 *
 * @tags: [requires_fcv_49, requires_persistence]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

let conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
const coll = testDB.getCollection('t');

const timeFieldName = 'time';
const metaFieldName = 'meta';

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

MongoRunner.stopMongod(conn);

// Restarting the server with a time-series collection present should startup, even though
// time-series collections do not have an _id index.
conn = MongoRunner.runMongod({dbpath: conn.dbpath, noCleanData: true});
assert(conn);
MongoRunner.stopMongod(conn);
})();
