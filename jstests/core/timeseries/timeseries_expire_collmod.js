/**
 * Tests that collMod can change a time-series bucket collections expireAfterSeconds option.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
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

const coll = db.timeseries_expire_collmod;
coll.drop();

const timeFieldName = 'time';
const expireAfterSeconds = NumberLong(5);
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSeconds}}));

const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

// Cannot use the 'clusteredIndex' option on collections that aren't time-series bucket collections.
const collNotClustered = db.getCollection(coll.getName() + '_not_clustered');
collNotClustered.drop();
assert.commandWorked(db.createCollection(collNotClustered.getName()));
assert.commandFailedWithCode(
    db.runCommand({collMod: collNotClustered.getName(), clusteredIndex: {expireAfterSeconds: 10}}),
    ErrorCodes.InvalidOptions);

// Check for invalid input.
assert.commandFailedWithCode(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: "10"}}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: {}}}),
    ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: -10}}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {}}),
                             40414);

let res = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(expireAfterSeconds, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);

// Change expireAfterSeconds to 10.
assert.commandWorked(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: 10}}));

res = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(10, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);

// Change expireAfterSeconds to 0.
assert.commandWorked(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: 0}}));

res = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(0, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);

// Disable expireAfterSeconds.
assert.commandWorked(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: "off"}}));

res = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert(!res.cursor.firstBatch[0].options.clusteredIndex.hasOwnProperty("expireAfterSeconds"));

// Enable expireAfterSeconds again.
assert.commandWorked(
    db.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: 100}}));

res = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(100, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);
})();
