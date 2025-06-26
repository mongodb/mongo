/**
 * Typically, time-series collections use measurements that always contain data for every field.
 * This test provides coverage for when this is not the case.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collNamePrefix = jsTestName() + '_';

    const timeFieldName = 'time';
    let collCount = 0;

    /**
     * Accepts two lists of measurements. The first list is used to create a new bucket. The second
     * list is used to append measurements to the new bucket. We should see one bucket created in
     * the time-series collection.
     */
    const runTest = function(docsInsert, docsUpdate) {
        const coll = db.getCollection(collNamePrefix + collCount++);
        coll.drop();

        jsTestLog('Running test: collection: ' + coll.getFullName() + '; initial measurements: ' +
                  tojson(docsInsert) + '; measurements to append: ' + tojson(docsUpdate));

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
        if (TestData.runningWithBalancer) {
            // In suites running moveCollection in the background, it is possible to hit the issue
            // described by SERVER-89349 which will result in more bucket documents being created.
            // Creating an index on the time field allows the buckets to be reopened, allowing the
            // counts in this test to be accurate.
            assert.commandWorked(coll.createIndex({[timeFieldName]: 1}));
        }

        assert.commandWorked(insert(coll, docsInsert),
                             'failed to create bucket with initial docs: ' + tojson(docsInsert));
        assert.commandWorked(insert(coll, docsUpdate),
                             'failed to append docs to bucket : ' + tojson(docsUpdate));

        // Check measurements.
        let docs = docsInsert.concat(docsUpdate);
        const userDocs = coll.find({}).sort({_id: 1}).toArray();
        assert.eq(docs.length, userDocs.length, userDocs);
        for (let i = 0; i < docs.length; i++) {
            assert.docEq(docs[i], userDocs[i], 'unexpected measurement doc: ' + i);
        }

        // Check buckets.
        const bucketDocs = getTimeseriesCollForRawOps(coll)
                               .find()
                               .rawData()
                               .sort({'control.min._id': 1})
                               .toArray();
        assert.eq(1, bucketDocs.length, bucketDocs);
        TimeseriesTest.decompressBucket(bucketDocs[0]);

        // Check bucket.
        assert.eq(docs.length,
                  Object.keys(bucketDocs[0].data[timeFieldName]).length,
                  'invalid number of measurements in first bucket: ' + tojson(bucketDocs[0]));
    };

    // Ensure _id order of raw buckets documents by using constant times.
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
});
