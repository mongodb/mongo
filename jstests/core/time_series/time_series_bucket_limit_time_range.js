/**
 * Tests maximum time-range of measurements held in each bucket in a time-series buckets collection.
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

// Assumes the measurements in each bucket span at most one hour (based on the time field).
const bucketMaxTimeRangeHours = 1;
const numDocs = 2;
const now = ISODate();
// Measurements should be more than 'bucketMaxTimeRangeHours' apart.
const docTimes = [ISODate("2020-11-13T01:00:00Z"), ISODate("2020-11-13T03:00:00Z")];

for (let i = 0; i < numDocs; i++) {
    const doc = {_id: i, [timeFieldName]: docTimes[i], x: i};

    assert.commandWorked(coll.insert(doc), 'failed to insert doc: ' + i + ': ' + tojson(doc));
}

// Check view.
const viewDocs = coll.find().sort({_id: 1}).toArray();
assert.eq(numDocs, viewDocs.length, viewDocs);
for (let i = 0; i < numDocs; i++) {
    const viewDoc = viewDocs[i];
    assert.eq(i, viewDoc._id, 'unexpected _id in doc: ' + i + ': ' + tojson(viewDoc));
    assert.eq(i, viewDoc.x, 'unexpected field x in doc: ' + i + ': ' + tojson(viewDoc));
    assert.eq(docTimes[i],
              viewDoc[timeFieldName],
              'unexpected time in doc: ' + i + ': ' + tojson(viewDoc));
}

// Check bucket collection.
const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(2, bucketDocs.length, bucketDocs);

// Check both buckets.
// First bucket should be not contain both documents because the time of the second measurement is
// ahead of the first document by more than 'bucketMaxTimeRangeHours'.
assert.eq(
    1, bucketDocs[0].control.count, 'invalid count in first bucket: ' + tojson(bucketDocs[0]));
assert.eq(0,
          bucketDocs[0].control.min._id,
          'invalid control.min for _id in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(0,
          bucketDocs[0].control.min.x,
          'invalid control.min for x in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(docTimes[0],
          bucketDocs[0].control.min[timeFieldName],
          'invalid control.min for time in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(0,
          bucketDocs[0].control.max._id,
          'invalid control.max for _id in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(0,
          bucketDocs[0].control.max.x,
          'invalid control.max for x in first bucket: ' + tojson(bucketDocs[0].control));
assert.eq(docTimes[0],
          bucketDocs[0].control.max[timeFieldName],
          'invalid control.max for time in first bucket: ' + tojson(bucketDocs[0].control));

// Second bucket should contain the remaining document.
assert.eq(
    1, bucketDocs[1].control.count, 'invalid count in second bucket: ' + tojson(bucketDocs[1]));
assert.eq(numDocs - 1,
          bucketDocs[1].control.min._id,
          'invalid control.min for _id in second bucket: ' + tojson(bucketDocs[1].control));
assert.eq(numDocs - 1,
          bucketDocs[1].control.min.x,
          'invalid control.min for x in second bucket: ' + tojson(bucketDocs[1].control));
assert.eq(docTimes[numDocs - 1],
          bucketDocs[1].control.min[timeFieldName],
          'invalid control.min for time in second bucket: ' + tojson(bucketDocs[1].control));
assert.eq(numDocs - 1,
          bucketDocs[1].control.max._id,
          'invalid control.max for _id in second bucket: ' + tojson(bucketDocs[1].control));
assert.eq(numDocs - 1,
          bucketDocs[1].control.max.x,
          'invalid control.max for x in second bucket: ' + tojson(bucketDocs[1].control));
assert.eq(docTimes[numDocs - 1],
          bucketDocs[1].control.max[timeFieldName],
          'invalid control.max for time in second bucket: ' + tojson(bucketDocs[1].control));
})();
