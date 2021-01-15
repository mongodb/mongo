/**
 * Tests that running collStats against a time-series collection includes statistics specific to
 * time-series collections.
 *
 * @tags: [
 *     assumes_unsharded_collection,         # TODO(SERVER-53816): remove
 *     does_not_support_causal_consistency,  # TODO(SERVER-53819): remove
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

const expectedStats = {
    ns: coll.getFullName(),
    bucketsNs: bucketsColl.getFullName(),
    bucketCount: 0,
    numBucketInserts: 0,
    numBucketUpdates: 0,
    numBucketsOpenedDueToMetadata: 0,
    numBucketsClosedDueToCount: 0,
    numBucketsClosedDueToSize: 0,
    numBucketsClosedDueToTimeForward: 0,
    numBucketsClosedDueToTimeBackward: 0,
    numCommits: 0,
    numWaits: 0,
    numMeasurementsCommitted: 0,
};

const checkCollStats = function(empty = false) {
    const stats = assert.commandWorked(coll.stats());

    for (let [stat, value] of Object.entries(expectedStats)) {
        assert.eq(
            stats[stat], value, "Invalid '" + stat + "' value in collStats: " + tojson(stats));
    }

    if (empty) {
        assert(!stats.hasOwnProperty('avgBucketSize'));
        assert(!stats.hasOwnProperty('avgNumMeasurementsPerCommit'));
    } else {
        assert.gt(stats.avgBucketSize, 0);
    }

    assert(!stats.hasOwnProperty('count'));
    assert(!stats.hasOwnProperty('avgObjSize'));
};

checkCollStats(true);

let docs = Array(2).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}});
assert.commandWorked(coll.insert(docs));
expectedStats.bucketCount++;
expectedStats.numBucketInserts++;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numCommits++;
expectedStats.numWaits++;
expectedStats.numMeasurementsCommitted += 2;
expectedStats.avgNumMeasurementsPerCommit = 2;
checkCollStats();

assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {a: 2}}));
expectedStats.bucketCount++;
expectedStats.numBucketInserts++;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numCommits++;
expectedStats.numMeasurementsCommitted++;
expectedStats.avgNumMeasurementsPerCommit = 1;
checkCollStats();

assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {a: 2}}));
expectedStats.numBucketUpdates++;
expectedStats.numCommits++;
expectedStats.numMeasurementsCommitted++;
checkCollStats();

docs = Array(5).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 2}});
assert.commandWorked(coll.insert(docs));
expectedStats.numBucketUpdates++;
expectedStats.numCommits++;
expectedStats.numWaits += 4;
expectedStats.numMeasurementsCommitted += 5;
expectedStats.avgNumMeasurementsPerCommit = 2;
checkCollStats();

assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: {a: 1}}));
expectedStats.bucketCount++;
expectedStats.numBucketInserts++;
expectedStats.numCommits++;
expectedStats.numBucketsClosedDueToTimeBackward++;
expectedStats.numMeasurementsCommitted++;
checkCollStats();

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
let numDocs = bucketMaxCount + 100;
docs = Array(numDocs).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 'limit_count'}});
assert.commandWorked(coll.insert(docs));
expectedStats.bucketCount += 2;
expectedStats.numBucketInserts += 2;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToCount++;
expectedStats.numCommits += 2;
expectedStats.numWaits += numDocs - 2;
expectedStats.numMeasurementsCommitted += numDocs;
expectedStats.avgNumMeasurementsPerCommit =
    Math.floor(expectedStats.numMeasurementsCommitted / expectedStats.numCommits);
checkCollStats();

// Assumes each bucket has a limit of 125kB on the measurements stored in the 'data' field.
const bucketMaxSizeKB = 125;
numDocs = 2;
// The measurement data should not take up all of the 'bucketMaxSizeKB' limit because we need
// to leave a little room for the _id and the time fields.
const largeValue = 'x'.repeat((bucketMaxSizeKB - 1) * 1024);
docs = Array(numDocs).fill(
    {[timeFieldName]: ISODate(), x: largeValue, [metaFieldName]: {a: 'limit_size'}});
assert.commandWorked(coll.insert(docs));
expectedStats.bucketCount += numDocs;
expectedStats.numBucketInserts += numDocs;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToSize++;
expectedStats.numCommits += numDocs;
expectedStats.numMeasurementsCommitted += numDocs;
expectedStats.avgNumMeasurementsPerCommit =
    Math.floor(expectedStats.numMeasurementsCommitted / expectedStats.numCommits);
checkCollStats();

// Assumes the measurements in each bucket span at most one hour (based on the time field).
const docTimes = [ISODate("2020-11-13T01:00:00Z"), ISODate("2020-11-13T03:00:00Z")];
numDocs = 2;
docs = [];
for (let i = 0; i < numDocs; i++) {
    docs.push({[timeFieldName]: docTimes[i], [metaFieldName]: {a: 'limit_time_range'}});
}
assert.commandWorked(coll.insert(docs));
expectedStats.bucketCount += numDocs;
expectedStats.numBucketInserts += numDocs;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToTimeForward++;
expectedStats.numCommits += numDocs;
expectedStats.numMeasurementsCommitted += numDocs;
expectedStats.avgNumMeasurementsPerCommit =
    Math.floor(expectedStats.numMeasurementsCommitted / expectedStats.numCommits);
checkCollStats();
})();
