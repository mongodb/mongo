/**
 * Tests 'metrics.timeseries' document is updated correctly in the serverStatus output.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('ts');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'tag';

const resetColl = function() {
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(coll.insert({a: 0, [timeFieldName]: new Date(), [metaFieldName]: 1}));
    assert.commandWorked(coll.insert({a: 1, [timeFieldName]: new Date(), [metaFieldName]: 2}));
    assert.commandWorked(coll.insert({a: 2, [timeFieldName]: new Date(), [metaFieldName]: 2}));
};

const expectedMetrics = {
    directDeleted: 0,
    directUpdated: 0,
    measurementDelete: 0,
    measurementUpdate: 0,
    metaDelete: 0,
    metaUpdate: 0,
};

const checkServerStatus = function() {
    const metrics = assert.commandWorked(testDB.serverStatus()).metrics.timeseries;

    for (let [metric, value] of Object.entries(expectedMetrics)) {
        assert.eq(metrics[metric], value, metric);
    }

    resetColl();
};

resetColl();

if (FeatureFlagUtil.isEnabled(testDB, "TimeseriesUpdatesSupport")) {
    // User update that removes the original bucket and insert into a new one.
    ++expectedMetrics.directDeleted;
    ++expectedMetrics.measurementUpdate;
    assert.commandWorked(coll.updateOne({[metaFieldName]: 1}, {$set: {b: 1}}));
    checkServerStatus();

    // User update that updates the original bucket and insert into a new one.
    ++expectedMetrics.directUpdated;
    ++expectedMetrics.measurementUpdate;
    assert.commandWorked(coll.updateOne({[metaFieldName]: 2}, {$set: {b: 1}}));
    checkServerStatus();
}

if (!FeatureFlagUtil.isEnabled(testDB, "TimeseriesUpdatesSupport")) {
    // User update that only queries and updates the meta field.
    ++expectedMetrics.directUpdated;
    ++expectedMetrics.metaUpdate;
    assert.commandWorked(coll.updateMany({[metaFieldName]: 1}, {$set: {[metaFieldName]: 2}}));
    checkServerStatus();
}

// Direct bucket update.
++expectedMetrics.directUpdated;
assert.commandWorked(bucketsColl.updateOne({meta: 1}, {$set: {meta: 3}}));
checkServerStatus();

// User delete that updates the original bucket.
++expectedMetrics.directUpdated;
++expectedMetrics.measurementDelete;
assert.commandWorked(coll.deleteOne({a: 1}));
checkServerStatus();

// User delete that removes the original bucket.
++expectedMetrics.directDeleted;
++expectedMetrics.measurementDelete;
assert.commandWorked(coll.deleteMany({a: 0}));
checkServerStatus();

// User delete that runs directly on the bucket level.
++expectedMetrics.directDeleted;
++expectedMetrics.metaDelete;
assert.commandWorked(coll.deleteMany({[metaFieldName]: 1}));
checkServerStatus();

// Direct bucket delete.
++expectedMetrics.directDeleted;
assert.commandWorked(bucketsColl.deleteOne({meta: 1}));
checkServerStatus();

MongoRunner.stopMongod(conn);
