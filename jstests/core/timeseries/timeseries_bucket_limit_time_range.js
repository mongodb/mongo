/**
 * Tests maximum time-range of measurements held in each bucket in a time-series buckets collection.
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

const timeFieldName = 'time';

// Assumes the measurements in each bucket span at most one hour (based on the time field).
const docTimes = [ISODate("2020-11-13T01:00:00Z"), ISODate("2020-11-13T03:00:00Z")];
const numDocs = 2;

const runTest = function(numDocsPerInsert) {
    const coll = testDB.getCollection('t_' + numDocsPerInsert);
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i, [timeFieldName]: docTimes[i], x: i});
        if ((i + 1) % numDocsPerInsert === 0) {
            assert.commandWorked(coll.insert(docs), 'failed to insert docs: ' + tojson(docs));
            docs = [];
        }
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
    // First bucket should be not contain both documents because the time of the second measurement
    // is ahead of the first document by more than 'bucketMaxTimeRangeHours'.
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
};

runTest(1);
runTest(numDocs);
})();
