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

const testDB = db.getSiblingDB("timeseries_expire_collmod");
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
coll.drop();

const timeFieldName = 'time';
const expireAfterSeconds = NumberLong(5);
assert.commandWorked(testDB.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSeconds}}));

const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

// Cannot use the 'clusteredIndex' option on collections that aren't time-series bucket collections.
assert.commandWorked(testDB.createCollection("other"));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: "other", clusteredIndex: {expireAfterSeconds: 10}}),
    ErrorCodes.InvalidOptions);

// Check for invalid input.
assert.commandFailedWithCode(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: "10"}}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: {}}}),
    ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: -10}}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {}}), 40414);

let res = assert.commandWorked(
    testDB.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(expireAfterSeconds, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);

// Change expireAfterSeconds to 10.
assert.commandWorked(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: 10}}));

res = assert.commandWorked(
    testDB.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(10, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);

// Change expireAfterSeconds to 0.
assert.commandWorked(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: 0}}));

res = assert.commandWorked(
    testDB.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(0, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);

// Disable expireAfterSeconds.
assert.commandWorked(testDB.runCommand(
    {collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: "off"}}));

res = assert.commandWorked(
    testDB.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert(!res.cursor.firstBatch[0].options.clusteredIndex.hasOwnProperty("expireAfterSeconds"));

// Enable expireAfterSeconds again.
assert.commandWorked(
    testDB.runCommand({collMod: bucketsColl.getName(), clusteredIndex: {expireAfterSeconds: 100}}));

res = assert.commandWorked(
    testDB.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(100, res.cursor.firstBatch[0].options.clusteredIndex.expireAfterSeconds);
})();
