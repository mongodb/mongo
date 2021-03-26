/**
 * Tests non-clustered time-series collections against collMod. Checks that changing
 * expireAfterSeconds on indexes is acceptable, but prevents changing the clustered
 * expireAfterSeconds option in the catalog.
 *
 * @tags: [
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

// Disable clustering by _id on time-series collections.
const conn = MongoRunner.runMongod({setParameter: 'timeseriesBucketsCollectionClusterById=false'});

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const db = conn.getDB("test");
const coll = db.timeseries_non_clustered_collmod;

// Builds an index on control.min.time.
const timeFieldName = 'time';
const expireAfterSeconds = NumberLong(5);
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSeconds}}));

let indexes = assert.commandWorked(db.runCommand({listIndexes: coll.getName()})).cursor.firstBatch;
assert.eq(1, indexes.length);
assert.eq(indexes[0].key, {time: 1});

// Changing expireAfterSeconds on control.min.time is acceptable.
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
assert.commandWorked(db.runCommand({
    collMod: bucketsColl.getName(),
    index: {keyPattern: {"control.min.time": 1}, expireAfterSeconds: NumberLong(20)}
}));

// Changing the clustered expireAfterSeconds option in the catalog can't happen on a non-clustered
// time-series collection.
assert.commandFailedWithCode(
    db.runCommand({collMod: coll.getName(), clusteredIndex: {expireAfterSeconds: NumberLong(10)}}),
    ErrorCodes.InvalidOptions);

MongoRunner.stopMongod(conn);
})();