/**
 * Typically, time-series collections use measurements that always contain data for every field.
 * This test provides coverage for when this is not the case.
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
let collCount = 0;

/**
 * Accepts two lists of measurements. The first list is used to create a new bucket. The second list
 * is used to append measurements to the new bucket. We should see one bucket created in the
 * time-series collection.
 */
const runTest = function(docsInsert, docsUpdate) {
    const coll = testDB.getCollection('t_' + collCount++);
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    jsTestLog('Running test: collection: ' + coll.getFullName() + '; bucket collection: ' +
              bucketsColl.getFullName() + '; initial measurements: ' + tojson(docsInsert) +
              '; measurements to append: ' + tojson(docsUpdate));

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    assert.commandWorked(coll.insert(docsInsert),
                         'failed to create bucket with initial docs: ' + tojson(docsInsert));
    assert.commandWorked(coll.insert(docsUpdate),
                         'failed to append docs to bucket : ' + tojson(docsUpdate));

    // Check view.
    let docs = docsInsert.concat(docsUpdate);
    const viewDocs = coll.find({}).sort({_id: 1}).toArray();
    assert.eq(docs.length, viewDocs.length, viewDocs);
    for (let i = 0; i < docs.length; i++) {
        assert.docEq(docs[i], viewDocs[i], 'unexpected doc from view: ' + i);
    }

    // Check bucket collection.
    const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
    assert.eq(1, bucketDocs.length, bucketDocs);

    // Check bucket.
    assert.eq(docs.length,
              Object.keys(bucketDocs[0].data[timeFieldName]).length,
              'invalid number of measurements in first bucket: ' + tojson(bucketDocs[0]));
};

// Ensure _id order of docs in the bucket collection by using constant times.
const t = [
    ISODate("2020-11-26T00:00:00.000Z"),
    ISODate("2020-11-26T00:10:00.000Z"),
    ISODate("2020-11-26T00:20:00.000Z"),
    ISODate("2020-11-26T00:30:00.000Z"),
];

// One field per measurement. No overlap in (non-time) data fields.
runTest(
    [
        {_id: 0, time: t[0], a: 0},
        {_id: 1, time: t[1], b: 10},
    ],
    [
        {_id: 2, time: t[2], c: 20},
        {_id: 3, time: t[3], d: 30},
    ]);

// Two fields per measurement. Fields overlaps with previous measurement in a sliding window.
runTest(
    [
        {_id: 0, time: t[0], a: 0, d: 100},
        {_id: 1, time: t[1], a: 11, b: 10},
    ],
    [
        {_id: 2, time: t[2], b: 22, c: 20},
        {_id: 3, time: t[3], c: 33, d: 30},
    ]);
})();
