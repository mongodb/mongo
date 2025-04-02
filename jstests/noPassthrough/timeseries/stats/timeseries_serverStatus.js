/**
 * Tests that serverStatus contains a bucketCatalog section.
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');

const timeFieldName = 'time';
const metaFieldName = 'meta';

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

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

const insertDoc = function(doc) {
    assert.commandWorked(coll.insert(doc, {ordered: false}));
};

checkNoServerStatus();

// Inserting the first measurement will open a new bucket.
expectedMetrics.numBuckets++;
expectedMetrics.numOpenBuckets++;
insertDoc({[timeFieldName]: ISODate("2025-03-19T05:17:00Z"), [metaFieldName]: {a: 1}});

// Once the insert is complete, the bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

// Insert a time-backward measurement.
expectedMetrics.numBuckets++;
expectedMetrics.numIdleBuckets--;
insertDoc({[timeFieldName]: ISODate("2021-01-02T01:00:00Z"), [metaFieldName]: {a: 1}});

// Once the insert is complete, the closed bucket goes away and the open bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

// Insert a measurement which will close/archive the existing bucket right away.
expectedMetrics.numIdleBuckets--;
expectedMetrics.numBuckets++;
insertDoc({[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: {a: 1}});

// Once the insert is complete, the new bucket becomes idle.
expectedMetrics.numIdleBuckets++;
checkServerStatus();

assert(coll.drop());

MongoRunner.stopMongod(conn);
