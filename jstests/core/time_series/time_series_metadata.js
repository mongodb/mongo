/**
 * Tests that only measurements with a binary identical meta field are included in the same bucket
 * in a time-series collection.
 * @tags: [
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
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

const timeFieldName = 'time';
const metaFieldName = 'meta';

let collCount = 0;

/**
 * Accepts two lists of measurements. Measurements in each list should contain the same value for
 * the metadata field (but distinct from the other list). We should see two buckets created in the
 * time-series collection.
 */
const runTest = function(docsBucketA, docsBucketB) {
    const coll = testDB.getCollection('t_' + collCount++);
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    jsTestLog('Running test: collection: ' + coll.getFullName() +
              '; bucket collection: ' + bucketsColl.getFullName() +
              '; bucketA: ' + tojson(docsBucketA) + '; bucketB: ' + tojson(docsBucketB));

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    let docs = docsBucketA.concat(docsBucketB);
    assert.commandWorked(coll.insert(docs), 'failed to insert docs: ' + tojson(docs));

    // Check view.
    const viewDocs = coll.find({}).sort({_id: 1}).toArray();
    assert.eq(docs.length, viewDocs.length, viewDocs);
    for (let i = 0; i < docs.length; i++) {
        assert.docEq(docs[i], viewDocs[i], 'unexpected doc from view: ' + i);
    }

    // Check bucket collection.
    const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
    assert.eq(2, bucketDocs.length, bucketDocs);

    // Check both buckets.
    // First bucket should contain documents specified in 'bucketA'.
    assert.eq(docsBucketA.length,
              Object.keys(bucketDocs[0].data[timeFieldName]).length,
              'invalid number of measurements in first bucket: ' + tojson(bucketDocs[0]));
    if (docsBucketA[0].hasOwnProperty(metaFieldName)) {
        assert.eq(docsBucketA[0][metaFieldName],
                  bucketDocs[0].control.meta,
                  'invalid control.meta in first bucket: ' + tojson(bucketDocs[0].control));
        assert(bucketDocs[0].data.hasOwnProperty(metaFieldName),
               'metadata missing first bucket data: ' + tojson(bucketDocs[0]));
    } else {
        assert(bucketDocs[0].control.hasOwnProperty('meta'),
               'missing control.meta in first bucket: ' + tojson(bucketDocs[0].control));
        assert.eq(null,
                  bucketDocs[0].control.meta,
                  'invalid control.meta for x in first bucket: ' + tojson(bucketDocs[0].control));
    }

    // Second bucket should contain documents specified in 'bucketB'.
    assert.eq(docsBucketB.length,
              Object.keys(bucketDocs[1].data[timeFieldName]).length,
              'invalid number of measurements in second bucket: ' + tojson(bucketDocs[1]));
    if (docsBucketB[0].hasOwnProperty(metaFieldName)) {
        assert.eq(docsBucketB[0][metaFieldName],
                  bucketDocs[1].control.meta,
                  'invalid control.meta in second bucket: ' + tojson(bucketDocs[1].control));
        assert(bucketDocs[1].data.hasOwnProperty(metaFieldName),
               'metadata missing second bucket data: ' + tojson(bucketDocs[1]));
    } else {
        assert(bucketDocs[1].control.hasOwnProperty('meta'),
               'missing control.meta in second bucket: ' + tojson(bucketDocs[1].control));
        assert.eq(null,
                  bucketDocs[1].control.meta,
                  'invalid control.meta for x in second bucket: ' + tojson(bucketDocs[1].control));
    }
};

// Ensure _id order of docs in the bucket collection by using constant times.
const t = [
    ISODate("2020-11-26T00:00:00.000Z"),
    ISODate("2020-11-26T00:10:00.000Z"),
    ISODate("2020-11-26T00:20:00.000Z"),
    ISODate("2020-11-26T00:30:00.000Z"),
];

runTest(
    [
        {_id: 0, time: t[0], meta: 'a', x: 0},
        {_id: 1, time: t[1], meta: 'a', x: 10},
    ],
    [
        {_id: 2, time: t[2], meta: 'b', x: 20},
        {_id: 3, time: t[3], meta: 'b', x: 30},
    ]);

runTest(
    // No metadata field in first bucket.
    [
        {_id: 0, time: t[0], x: 0},
        {_id: 1, time: t[1], x: 10},
    ],
    [
        {_id: 2, time: t[2], meta: 123, x: 20},
        {_id: 3, time: t[3], meta: 123, x: 30},
    ]);

runTest(
    // Metadata field contains an array.
    [
        {_id: 0, time: t[0], meta: [1, 2, 3], x: 0},
        {_id: 1, time: t[1], meta: [1, 2, 3], x: 10},
    ],
    // No metadata field in second bucket.
    [
        {_id: 2, time: t[2], x: 20},
        {_id: 3, time: t[3], x: 30},
    ]);

runTest(
    // Metadata field contains an object.
    [
        {_id: 0, time: t[0], meta: {a: 1, b: 1}, x: 0},
        {_id: 1, time: t[1], meta: {a: 1, b: 1}, x: 10},
    ],
    // Metadata field contains the same object as the first bucket's contents in a different order.
    // TODO(SERVER-52967): These measurements should go into the first bucket.
    [
        {_id: 2, time: t[2], meta: {b: 1, a: 1}, x: 20},
        {_id: 3, time: t[3], meta: {b: 1, a: 1}, x: 30},
    ]);
})();
