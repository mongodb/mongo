/**
 * Tests that only measurements with a binary identical meta field are included in the same bucket
 * in a time-series collection.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

TimeseriesTest.run((insert) => {
    const collNamePrefix = 'timeseries_metadata_';

    const timeFieldName = 'time';
    const metaFieldName = 'meta';

    let collCount = 0;

    /**
     * Accepts two lists of measurements. Measurements in each list should contain the same value
     * for the metadata field (but distinct from the other list). We should see two buckets created
     * in the time-series collection.
     */
    const runTest = function(docsBucketA, docsBucketB) {
        const coll = db.getCollection(collNamePrefix + collCount++);
        const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
        coll.drop();

        jsTestLog('Running test: collection: ' + coll.getFullName() +
                  '; bucket collection: ' + bucketsColl.getFullName() +
                  '; bucketA: ' + tojson(docsBucketA) + '; bucketB: ' + tojson(docsBucketB));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.contains(bucketsColl.getName(), db.getCollectionNames());

        let docs = docsBucketA.concat(docsBucketB);
        assert.commandWorked(insert(coll, docs), 'failed to insert docs: ' + tojson(docs));

        // Check view.
        const viewDocs = coll.find({}).sort({_id: 1}).toArray();
        assert.eq(docs.length, viewDocs.length, viewDocs);
        for (let i = 0; i < docs.length; i++) {
            assert.docEq(docs[i], viewDocs[i], 'unexpected doc from view: ' + i);
        }

        // Check bucket collection.
        const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();
        assert.eq(2, bucketDocs.length, bucketDocs);

        // Check both buckets.
        // First bucket should contain documents specified in 'bucketA'.
        assert.eq(docsBucketA.length,
                  Object.keys(bucketDocs[0].data[timeFieldName]).length,
                  'invalid number of measurements in first bucket: ' + tojson(bucketDocs[0]));
        if (docsBucketA[0].hasOwnProperty(metaFieldName)) {
            assert.eq(docsBucketA[0][metaFieldName],
                      bucketDocs[0].meta,
                      'invalid meta in first bucket: ' + tojson(bucketDocs[0]));
            assert(!bucketDocs[0].data.hasOwnProperty(metaFieldName),
                   'unexpected metadata in first bucket data: ' + tojson(bucketDocs[0]));
        }

        // Second bucket should contain documents specified in 'bucketB'.
        assert.eq(docsBucketB.length,
                  Object.keys(bucketDocs[1].data[timeFieldName]).length,
                  'invalid number of measurements in second bucket: ' + tojson(bucketDocs[1]));
        if (docsBucketB[0].hasOwnProperty(metaFieldName)) {
            assert.eq(docsBucketB[0][metaFieldName],
                      bucketDocs[1].meta,
                      'invalid meta in second bucket: ' + tojson(bucketDocs[1]));
            assert(!bucketDocs[1].data.hasOwnProperty(metaFieldName),
                   'unexpected metadata in second bucket data: ' + tojson(bucketDocs[1]));
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
        [
            {_id: 0, time: t[0], meta: null, x: 0},
            {_id: 1, time: t[1], meta: null, x: 10},
        ],
        [
            {_id: 2, time: t[2], x: 20},
            {_id: 3, time: t[3], x: 30},
        ]);

    runTest(
        [
            {_id: 0, time: t[0], meta: null, x: 0},
            {_id: 1, time: t[1], meta: null, x: 10},
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
        [
            {_id: 2, time: t[2], x: 20},
            {_id: 3, time: t[3], x: 30},
        ]);

    runTest(
        // Metadata field contains an object. Its ordering does not affect which bucket is used.
        [
            {_id: 0, time: t[0], meta: {a: 1, b: 1}, x: 0},
            {_id: 1, time: t[1], meta: {a: 1, b: 1}, x: 10},
            {_id: 2, time: t[2], meta: {b: 1, a: 1}, x: 20},
        ],
        [
            {_id: 3, time: t[3], meta: {a: 1}, x: 30},
        ]);

    runTest(
        // Metadata field contains an array within an object.
        [
            {_id: 0, time: t[0], meta: {a: [{b: 1, c: 0}]}, x: 0},
            {_id: 1, time: t[1], meta: {a: [{c: 0, b: 1}]}, x: 10},
        ],
        [
            {_id: 2, time: t[2], meta: {a: [{b: 2, c: 0}]}, x: 20},
            {_id: 3, time: t[3], meta: {a: [{c: 0, b: 2}]}, x: 30},
        ]);

    runTest(
        // Metadata field contains a nested array.
        [
            {_id: 0, time: t[0], meta: {a: [{b: 1, c: 0}, [{e: 1, f: 0}]]}, x: 0},
            {_id: 1, time: t[1], meta: {a: [{c: 0, b: 1}, [{f: 0, e: 1}]]}, x: 10},
        ],
        [
            {_id: 2, time: t[2], meta: {a: [[{e: 1, f: 0}], {b: 1, c: 0}]}, x: 20},
            {_id: 3, time: t[3], meta: {a: [[{f: 0, e: 1}], {c: 0, b: 1}]}, x: 30},
        ]);

    runTest(
        // Metadata field contains an array.
        [
            {_id: 0, time: t[0], meta: {a: [1, 2, 3]}, x: 0},
            {_id: 1, time: t[1], meta: {a: [1, 2, 3]}, x: 10},
        ],
        [
            {_id: 2, time: t[2], meta: {a: [2, 1, 3]}, x: 20},
            {_id: 3, time: t[3], meta: {a: [2, 1, 3]}, x: 30},
        ]);
});
})();
