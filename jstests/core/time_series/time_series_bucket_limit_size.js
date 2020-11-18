/**
 * Tests maximum size of measurements held in each bucket in a time-series buckets collection.
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
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

const controlVersion = 1;

// Assumes each bucket has a limit of 125kB on the measurements stored in the 'data' field.
const bucketMaxSizeKB = 125;
const numDocs = 2;

// The measurement data should not take up all of the 'bucketMaxSizeKB' limit because we need
// to leave a little room for the _id and the time fields.
const largeValue = 'x'.repeat((bucketMaxSizeKB - 1) * 1024);

for (let i = 0; i < numDocs; i++) {
    const t = ISODate();
    const doc = {_id: i, [timeFieldName]: t, x: largeValue};

    assert.commandWorked(coll.insert(doc), 'failed to insert doc: ' + i + ': ' + tojson(doc));
}

// Check view.
const viewDocs = coll.find({}, {x: 1}).sort({_id: 1}).toArray();
assert.eq(numDocs, viewDocs.length, viewDocs);
for (let i = 0; i < numDocs; i++) {
    const viewDoc = viewDocs[i];
    assert.eq(i, viewDoc._id, 'unexpected _id in doc: ' + i + ': ' + tojson(viewDoc));
    assert.eq(largeValue, viewDoc.x, 'unexpected field x in doc: ' + i + ': ' + tojson(viewDoc));
}

// Check bucket collection.
const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(1, bucketDocs.length, bucketDocs);

// Check both buckets.
// First bucket should be full with one document since we spill the second document over into the
// second bucket due to size constraints on 'data'.
assert.eq(numDocs,
          bucketDocs[0].control.count,
          'invalid count in first bucket: ' + tojson(bucketDocs[0]));
assert.eq(0,
          bucketDocs[0].control.min._id,
          'invalid control.min for _id in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(largeValue,
          bucketDocs[0].control.min.x,
          'invalid control.min for x in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(numDocs - 1,
          bucketDocs[0].control.max._id,
          'invalid control.max for _id in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(largeValue,
          bucketDocs[0].control.max.x,
          'invalid control.max for x in first bucket: ' + tojson(bucketDocs[0].control));
})();
