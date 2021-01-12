/**
 * Tests maximum size of measurements held in each bucket in a time-series buckets collection.
 * @tags: [
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
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

// Assumes each bucket has a limit of 125kB on the measurements stored in the 'data' field.
const bucketMaxSizeKB = 125;
const numDocs = 2;

// The measurement data should not take up all of the 'bucketMaxSizeKB' limit because we need
// to leave a little room for the _id and the time fields.
const largeValue = 'x'.repeat((bucketMaxSizeKB - 1) * 1024);

const runTest = function(numDocsPerInsert) {
    const coll = testDB.getCollection('t_' + numDocsPerInsert);
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i, [timeFieldName]: ISODate(), x: largeValue});
        if ((i + 1) % numDocsPerInsert === 0) {
            assert.commandWorked(coll.insert(docs), 'failed to insert docs: ' + tojson(docs));
            docs = [];
        }
    }

    // Check view.
    const viewDocs = coll.find({}, {x: 1}).sort({_id: 1}).toArray();
    assert.eq(numDocs, viewDocs.length, viewDocs);
    for (let i = 0; i < numDocs; i++) {
        const viewDoc = viewDocs[i];
        assert.eq(i, viewDoc._id, 'unexpected _id in doc: ' + i + ': ' + tojson(viewDoc));
        assert.eq(
            largeValue, viewDoc.x, 'unexpected field x in doc: ' + i + ': ' + tojson(viewDoc));
    }

    // Check bucket collection.
    const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
    assert.eq(2, bucketDocs.length, bucketDocs);

    // Check both buckets.
    // First bucket should be full with one document since we spill the second document over into
    // the second bucket due to size constraints on 'data'.
    assert.eq(0,
              bucketDocs[0].control.min._id,
              'invalid control.min for _id in first bucket: ' + tojson(bucketDocs[0].control));
    assert.eq(largeValue,
              bucketDocs[0].control.min.x,
              'invalid control.min for x in first bucket: ' + tojson(bucketDocs[0].control));
    assert.eq(0,
              bucketDocs[0].control.max._id,
              'invalid control.max for _id in first bucket: ' + tojson(bucketDocs[0].control));
    assert.eq(largeValue,
              bucketDocs[0].control.max.x,
              'invalid control.max for x in first bucket: ' + tojson(bucketDocs[0].control));

    // Second bucket should contain the remaining document.
    assert.eq(numDocs - 1,
              bucketDocs[1].control.min._id,
              'invalid control.min for _id in second bucket: ' + tojson(bucketDocs[1].control));
    assert.eq(largeValue,
              bucketDocs[1].control.min.x,
              'invalid control.min for x in second bucket: ' + tojson(bucketDocs[1].control));
    assert.eq(numDocs - 1,
              bucketDocs[1].control.max._id,
              'invalid control.max for _id in second bucket: ' + tojson(bucketDocs[1].control));
    assert.eq(largeValue,
              bucketDocs[1].control.max.x,
              'invalid control.max for x in second bucket: ' + tojson(bucketDocs[1].control));

    const stats = assert.commandWorked(coll.stats());
    assert.eq(stats.numBucketsClosedDueToSize,
              1,
              'invalid numBucketsClosedDueToSize in collStats: ' + tojson(stats));
};

runTest(1);
runTest(numDocs);
})();
