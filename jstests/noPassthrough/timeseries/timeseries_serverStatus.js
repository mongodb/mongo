/**
 * Tests that serverStatus contains a bucketCatalog section.
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();

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
    const metrics = assert.commandWorked(testDB.serverStatus()).bucketCatalog;

    const invalidMetricMsg = function(metric) {
        return "Invalid '" + metric + "' value in serverStatus: " + tojson(metrics);
    };

    for (let [metric, value] of Object.entries(expectedMetrics)) {
        assert.eq(metrics[metric], value, invalidMetricMsg(metric));
    }

    assert.gt(metrics.memoryUsage, 0, invalidMetricMsg('memoryUsage'));
};

const checkNoServerStatus = function() {
    const serverStatus = assert.commandWorked(testDB.serverStatus());
    assert(!serverStatus.hasOwnProperty('bucketCatalog'),
           'Found unexpected bucketCatalog section in serverStatus: ' +
               tojson(serverStatus.bucketCatalog));
};

const testWithInsertPaused = function(docs) {
    const fp = configureFailPoint(conn, "hangTimeseriesInsertBeforeCommit");

    const awaitInsert = startParallelShell(
        funWithArgs(function(dbName, collName, docs) {
            assert.commandWorked(
                db.getSiblingDB(dbName).getCollection(collName).insert(docs, {ordered: false}));
        }, dbName, coll.getName(), docs), conn.port);

    fp.wait();
    checkServerStatus();
    fp.off();

    awaitInsert();
};

checkNoServerStatus();

// Inserting the first measurement will open a new bucket.
expectedMetrics.numBuckets++;
expectedMetrics.numOpenBuckets++;
testWithInsertPaused({[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}});

// Once the insert is complete, the bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

// If the timeseriesAlwaysUseCompressedBuckets feature flag is enabled, when searching through
// candidate buckets useBucket also checks if the time range for the measurement that we are
// trying to insert matches the candidate bucket - if it does not, we do not return it. Because
// of this extra check, we do not attempt to insert a measurement into a bucket with an
// incompatible time range, which prevents that bucket from being rolled over. The following
// two checkServerStatus calls rely on the fact that inserting one measurement outside of the
// the time range of the existing bucket for that metadata will either soft close or archive
// the existing bucket, which will no longer be true under the feature flag.
// TODO SERVER-79481: Revisit this once we define an upper bound for the number of
// multiple open buckets per metadata, at which point buckets will rollover once again.
if (!TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(testDB)) {
    // Insert two measurements: one which will go into the existing bucket and a second which will
    // close that existing bucket. Thus, until the measurements are committed, the number of buckets
    // is than the number of open buckets.
    expectedMetrics.numBuckets++;
    expectedMetrics.numIdleBuckets--;
    testWithInsertPaused([
        {[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}},
        {[timeFieldName]: ISODate("2021-01-02T01:00:00Z"), [metaFieldName]: {a: 1}}
    ]);

    // Once the insert is complete, the closed bucket goes away and the open bucket becomes idle.
    expectedMetrics.numIdleBuckets++;
    checkServerStatus();

    // Insert a measurement which will close/archive the existing bucket right away.
    expectedMetrics.numIdleBuckets--;
    expectedMetrics.numBuckets++;
    testWithInsertPaused(
        {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: {a: 1}});

    // Once the insert is complete, the new bucket becomes idle.
    expectedMetrics.numIdleBuckets++;
    checkServerStatus();
}

assert(coll.drop());

MongoRunner.stopMongod(conn);