/**
 * Tests creating and dropping timeseries bucket collections and view definitions. Tests that we can
 * recover in both create and drop if a partial create occured where we have a bucket collection but
 * no view definition.
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const coll = db.timeseries_create_drop;
const viewName = coll.getName();
const viewNs = coll.getFullName();

// Disable test if fail point is missing (running in multiversion suite)
const failpoint = 'failTimeseriesViewCreation';
if (db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn", data: {ns: viewNs}}).ok ===
    0) {
    jsTestLog("Skipping test because the " + failpoint + " fail point is missing");
    return;
}
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));

const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
const bucketsCollName = bucketsColl.getName();
const timeFieldName = 'time';
const expireAfterSecondsNum = 60;

coll.drop();

// Create should create both bucket collection and view
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSecondsNum}}));
assert.contains(viewName, db.getCollectionNames());
assert.contains(bucketsCollName, db.getCollectionNames());

// Drop should drop both bucket collection and view
assert(coll.drop());
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.eq(db.getCollectionNames().findIndex(c => c == bucketsCollName), -1);

// Enable failpoint to allow bucket collection to be created but fail creation of view definition
assert.commandWorked(
    db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn", data: {ns: viewNs}}));
assert.commandFailed(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSecondsNum}}));
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, db.getCollectionNames());

// Dropping a partially created timeseries where only the bucket collection exists is allowed and
// should clean up the bucket collection
assert(coll.drop());
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.eq(db.getCollectionNames().findIndex(c => c == bucketsCollName), -1);

// Trying to create again yields the same result as fail point is still enabled
assert.commandFailed(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSecondsNum}}));
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, db.getCollectionNames());

// Turn off fail point and test creating view definition with existing bucket collection
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));

// Different timeField should fail
assert.commandFailed(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName + "2", expireAfterSeconds: expireAfterSecondsNum}}));
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, db.getCollectionNames());

// Different expireAfterSeconds should fail
assert.commandFailed(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSecondsNum + 1}}));
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, db.getCollectionNames());

// Omitting expireAfterSeconds should fail
assert.commandFailed(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.eq(db.getCollectionNames().findIndex(c => c == viewName), -1);
assert.contains(bucketsCollName, db.getCollectionNames());

// Same parameters should succeed
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSecondsNum}}));
assert.contains(viewName, db.getCollectionNames());
assert.contains(bucketsCollName, db.getCollectionNames());
})();
