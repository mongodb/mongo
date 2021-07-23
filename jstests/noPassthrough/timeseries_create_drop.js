/**
 * Tests creating and dropping timeseries bucket collections and view definitions. Tests that we can
 * recover in both create and drop if a partial create occured where we have a bucket collection but
 * no view definition.
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();
const primary = rst.getPrimary();
const primaryDb = primary.getDB('test');
if (!TimeseriesTest.timeseriesCollectionsEnabled(primaryDb.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const coll = primaryDb.timeseries_create_drop;
const viewName = coll.getName();
const viewNs = coll.getFullName();

// Disable test if fail point is missing (running in multiversion suite)
const failpoint = 'failTimeseriesViewCreation';
if (primaryDb.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn", data: {ns: viewNs}})
        .ok === 0) {
    jsTestLog("Skipping test because the " + failpoint + " fail point is missing");
    return;
}
assert.commandWorked(primaryDb.adminCommand({configureFailPoint: failpoint, mode: "off"}));

const bucketsColl = primaryDb.getCollection('system.buckets.' + coll.getName());
const bucketsCollName = bucketsColl.getName();
const timeFieldName = 'time';
const expireAfterSecondsNum = 60;

coll.drop();

// Create should create both bucket collection and view
assert.commandWorked(primaryDb.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSecondsNum}));
assert.contains(viewName, primaryDb.getCollectionNames());
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Drop should drop both bucket collection and view
assert(coll.drop());
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == bucketsCollName), -1);

// Enable failpoint to allow bucket collection to be created but fail creation of view definition
assert.commandWorked(
    primaryDb.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn", data: {ns: viewNs}}));
assert.commandFailed(primaryDb.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSecondsNum}));
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Dropping a partially created timeseries where only the bucket collection exists is allowed and
// should clean up the bucket collection
assert(coll.drop());
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == bucketsCollName), -1);

// Trying to create again yields the same result as fail point is still enabled
assert.commandFailed(primaryDb.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSecondsNum}));
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Turn off fail point and test creating view definition with existing bucket collection
assert.commandWorked(primaryDb.adminCommand({configureFailPoint: failpoint, mode: "off"}));

// Different timeField should fail
assert.commandFailed(primaryDb.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName + "2"}, expireAfterSeconds: expireAfterSecondsNum}));
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Different expireAfterSeconds should fail
assert.commandFailed(primaryDb.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSecondsNum + 1}));
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Omitting expireAfterSeconds should fail
assert.commandFailed(
    primaryDb.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.eq(primaryDb.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Same parameters should succeed
assert.commandWorked(primaryDb.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSecondsNum}));
assert.contains(viewName, primaryDb.getCollectionNames());
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

rst.stopSet();
})();
