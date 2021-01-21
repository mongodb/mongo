/**
 * Tests that serverStatus contains a bucketCatalog section.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

const expectedMetrics = {
    numBuckets: 0,
    numOpenBuckets: 0,
    numIdleBuckets: 0,
};

const checkServerStatus = function() {
    const metrics = assert.commandWorked(testDB.serverStatus({bucketCatalog: 1})).bucketCatalog;

    for (let [metric, value] of Object.entries(expectedMetrics)) {
        assert.eq(metrics[metric],
                  value,
                  "Invalid '" + metric + "' value in serverStatus: " + tojson(metrics));
    }
};

const testWithInsertPaused = function(docs) {
    const fp = configureFailPoint(conn, "hangTimeseriesInsertBeforeCommit");

    const awaitInsert = startParallelShell(
        funWithArgs(function(dbName, collName, docs) {
            assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).insert(docs));
        }, dbName, coll.getName(), docs), conn.port);

    fp.wait();
    checkServerStatus();
    fp.off();

    awaitInsert();
};

checkServerStatus();

// Inserting the first measurement will open a new bucket.
expectedMetrics.numBuckets++;
expectedMetrics.numOpenBuckets++;
testWithInsertPaused({[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}});

// Once the insert is complete, the bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

// Insert two measurements: one which will go into the existing bucket and a second which will close
// that existing bucket. Thus, until the measurements are committed, the number of buckets is
// than the number of open buckets.
expectedMetrics.numBuckets++;
expectedMetrics.numIdleBuckets--;
testWithInsertPaused([
    {[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}},
    {[timeFieldName]: ISODate("2021-01-02T01:00:00Z"), [metaFieldName]: {a: 1}}
]);

// Once the insert is complete, the closed bucket goes away and the open bucket becomes idle.
expectedMetrics.numBuckets--;
expectedMetrics.numIdleBuckets++;
checkServerStatus();

// Insert a measurement which will close the existing bucket right away.
expectedMetrics.numIdleBuckets--;
testWithInsertPaused({[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: {a: 1}});

// Once the insert is complete, the new bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

MongoRunner.stopMongod(conn);
})();