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

// Insert two measurements: one which will go into the existing bucket and a second which will
// close that existing bucket. Thus, until the measurements are committed, the number of buckets
// is greater than the number of open buckets.
expectedMetrics.numBuckets++;
expectedMetrics.numIdleBuckets--;

testWithInsertPaused([
    {[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}},
    {[timeFieldName]: ISODate("2021-01-02T01:00:00Z"), [metaFieldName]: {a: 1}}
]);

// When the timeseriesAlwaysUseCompressedBuckets feature flag is enabled, we will not attempt to
// insert measurements into buckets with incompatible time ranges. We instead search our
// list of open buckets for a particular metadata (with the flag on, there can be multiple open
// buckets per metadata) until we find a bucket that we can safely insert into (taking time range
// into account). We only close/rollover buckets if we have no open buckets we can safely insert a
// measurement into, and if we are at the maximum amount of buckets allowed per metadata. The
// default value is 5, which this test does not hit - therefore, none of the buckets get rolled
// over, which means that buckets that would previously be rolled over and closed, rather than left
// idle. This is why when the feature flag is enabled we expect a higher idle bucket count. With the
// feature flag off, once the insert is complete, the closed bucket goes away and the open bucket
// becomes idle.
// At the moment, the server status's numOpenBuckets relies on the fact that there is one open
// bucket per metadata. TODO SERVER-84680: Revisit this when we are updating stats and check that
// the number open buckets matches what is expected.
expectedMetrics.numIdleBuckets++;
if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(testDB)) {
    expectedMetrics.numIdleBuckets++;
}
checkServerStatus();

// Insert a measurement which will close/archive the existing bucket right away.
// If the feature flag is enabled, this insertion does not close/archive the bucekt right away,
// so we do not decrement our numIdleBucket counter.
if (!TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(testDB)) {
    expectedMetrics.numIdleBuckets--;
}
expectedMetrics.numBuckets++;
testWithInsertPaused({[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: {a: 1}});

// Once the insert is complete, the new bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

assert(coll.drop());

MongoRunner.stopMongod(conn);
