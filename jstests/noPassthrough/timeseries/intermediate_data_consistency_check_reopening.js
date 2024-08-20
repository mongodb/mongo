/**
 * Test that when we encounter a data integrity check failure while inserting a measurement
 * into a compressed bucket, that we successfully detect this failure, freeze the corrupted
 * bucket, and insert into a new bucket.
 *
 * @tags: [
 *  requires_timeseries,
 *  featureFlagTimeseriesAlwaysUseCompressedBuckets,
 *  requires_fcv_80,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Turn off the TestingProctor, since the data integrity check will invariant in testing
// but not in production.
TestData.testingDiagnosticsEnabled = false;
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "ts";
const timeFieldName = "time";
const testDB = rst.getPrimary().getDB(dbName);
const coll = testDB[collName];

const measurements = [
    {_id: 0, [timeFieldName]: ISODate("2024-02-15T10:10:10.000Z"), a: 1},
    {_id: 1, [timeFieldName]: ISODate("2024-02-15T08:10:20.000Z"), a: 2},
    {_id: 2, [timeFieldName]: ISODate("2024-02-15T10:10:20.000Z"), a: 3}
];

function testIntegrityCheck(turnFailpointOn) {
    jsTestLog("turnFailpointOn {" + turnFailpointOn + "}");

    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    // Insert first measurement, creating our first bucket A.
    assert.commandWorked(coll.insert(measurements[0]));

    // Insert second measurement. This should archive the existing bucket due to kTimeBackward,
    // and create a second bucket B to insert this measurement into.
    assert.commandWorked(coll.insert(measurements[1]));

    let stats = assert.commandWorked(coll.stats());
    assert.eq(stats.timeseries.numBucketsArchivedDueToTimeBackward, 1, tojson(stats.timeseries));

    if (turnFailpointOn) {
        // Turn on the failpoint that causes the timeseries data integrity check to fail.
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: 'timeseriesDataIntegrityCheckFailureUpdate', mode: {times: 1}}));

        // Insert third measurement - this should cause the first bucket A that we closed to be
        // reopened. We should try to insert into this bucket, but then fail when we try to add
        // on to it because of the failpoint. After the check fails we should freeze the first
        // bucket and insert into a new bucket C.
        assert.commandWorked(coll.insert(measurements[2]));

        stats = assert.commandWorked(coll.stats());
        assert.eq(stats.timeseries.numBucketsReopened, 1, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketsFrozen, 1, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketInserts, 3, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketsFetched, 1, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketsClosedDueToReopening, 1, tojson(stats.timeseries));
    } else {
        // Insert third measurement.
        assert.commandWorked(coll.insert(measurements[2]));
        stats = assert.commandWorked(coll.stats());
        assert.eq(stats.timeseries.numBucketsReopened, 1, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketsFrozen, 0, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketInserts, 2, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketsFetched, 1, tojson(stats.timeseries));
        assert.eq(stats.timeseries.numBucketsClosedDueToReopening, 1, tojson(stats.timeseries));
    }
}

testIntegrityCheck(/*turnFailpointOn=*/ false);
testIntegrityCheck(/*turnFailPointOn=*/ true);
rst.stopSet();
