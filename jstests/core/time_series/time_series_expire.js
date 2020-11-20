/**
 * Tests that a time-series collection created with the 'expireAfterSeconds' option will remove
 * buckets older than 'expireAfterSeconds' based on the bucket creation time.
 * @tags: [
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/time_series/libs/time_series.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

coll.drop();

const timeFieldName = 'time';
const expireAfterSeconds = NumberLong(5);
assert.commandWorked(testDB.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName, expireAfterSeconds: expireAfterSeconds}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

// Inserts a measurement with a time in the past to ensure the measurement will be removed
// immediately.
const t = ISODate("2020-11-13T01:00:00Z");
let start = ISODate();
assert.lt(t, start);

const doc = {
    _id: 0,
    [timeFieldName]: t,
    x: 0
};
assert.commandWorked(coll.insert(doc), 'failed to insert doc: ' + tojson(doc));
jsTestLog('Insertion took ' + ((new Date()).getTime() - start.getTime()) + ' ms.');

// Wait for the document to be removed.
start = ISODate();
assert.soon(() => {
    return 0 == coll.find().itcount();
});
jsTestLog('Removal took ' + ((new Date()).getTime() - start.getTime()) + ' ms.');

// Check bucket collection.
const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(0, bucketDocs.length, bucketDocs);
})();
