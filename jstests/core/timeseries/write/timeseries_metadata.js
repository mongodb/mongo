/**
 * Tests that only measurements with a binary identical meta field are included in the same bucket
 * in a time-series collection.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # We assume that all nodes in a mixed-mode replica set are using compressed inserts to a
 *   # time-series collection.
 *   requires_fcv_71,
 * ]
 */
import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collNamePrefix = jsTestName() + "_";

    const timeFieldName = "time";
    const metaFieldName = "m";

    let collCount = 0;

    /**
     * Accepts two lists of measurements. Measurements in each list should contain the same value
     * for the metadata field (but distinct from the other list). We should see two buckets created
     * in the time-series collection.
     */
    const runTest = function (docsBucketA, docsBucketB) {
        const coll = db.getCollection(collNamePrefix + collCount++);
        coll.drop();

        jsTestLog(
            "Running test: collection: " +
                coll.getFullName() +
                "; bucketA: " +
                tojson(docsBucketA) +
                "; bucketB: " +
                tojson(docsBucketB),
        );

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
        );

        let docs = docsBucketA.concat(docsBucketB);
        assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));

        // Check measurements.
        const userDocs = coll.find({}).sort({_id: 1}).toArray();
        assert.eq(docs.length, userDocs.length, userDocs);
        for (let i = 0; i < docs.length; i++) {
            assert.docEq(docs[i], userDocs[i], "unexpected measurement doc: " + i);
        }

        // Check buckets.
        const bucketDocs = getTimeseriesCollForRawOps(coll).find().rawData().sort({"control.min._id": 1}).toArray();
        assert.eq(2, bucketDocs.length, bucketDocs);

        // Check both buckets.
        // First bucket should contain documents specified in 'bucketA'. In order to verify that the
        // buckets contain the correct documents, we need to decompress the buckets.
        TimeseriesTest.decompressBucket(bucketDocs[0]);
        TimeseriesTest.decompressBucket(bucketDocs[1]);
        assert.eq(
            docsBucketA.length,
            Object.keys(bucketDocs[0].data[timeFieldName]).length,
            "invalid number of measurements in first bucket: " + tojson(bucketDocs[0]),
        );
        if (docsBucketA[0].hasOwnProperty(metaFieldName)) {
            assert.eq(
                docsBucketA[0][metaFieldName],
                bucketDocs[0].meta,
                "invalid meta in first bucket: " + tojson(bucketDocs[0]),
            );
            assert(
                !bucketDocs[0].data.hasOwnProperty(metaFieldName),
                "unexpected metadata in first bucket data: " + tojson(bucketDocs[0]),
            );
        }

        // Second bucket should contain documents specified in 'bucketB'.
        assert.eq(
            docsBucketB.length,
            Object.keys(bucketDocs[1].data[timeFieldName]).length,
            "invalid number of measurements in second bucket: " + tojson(bucketDocs[1]),
        );
        if (docsBucketB[0].hasOwnProperty(metaFieldName)) {
            assert.eq(
                docsBucketB[0][metaFieldName],
                bucketDocs[1].meta,
                "invalid meta in second bucket: " + tojson(bucketDocs[1]),
            );
            assert(
                !bucketDocs[1].data.hasOwnProperty(metaFieldName),
                "unexpected metadata in second bucket data: " + tojson(bucketDocs[1]),
            );
        }
    };

    // Ensure _id order of raw buckets documents by using constant times.
    const t = [
        ISODate("2020-11-26T00:00:00.000Z"),
        ISODate("2020-11-26T00:10:00.000Z"),
        ISODate("2020-11-26T00:20:00.000Z"),
        ISODate("2020-11-26T00:30:00.000Z"),
    ];

    runTest(
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: "a", x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: "a", x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], [metaFieldName]: "b", x: 20},
            {_id: 3, [timeFieldName]: t[3], [metaFieldName]: "b", x: 30},
        ],
    );

    runTest(
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: null, x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: null, x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], x: 20},
            {_id: 3, [timeFieldName]: t[3], x: 30},
        ],
    );

    runTest(
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: null, x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: null, x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], [metaFieldName]: 123, x: 20},
            {_id: 3, [timeFieldName]: t[3], [metaFieldName]: 123, x: 30},
        ],
    );

    runTest(
        // Metadata field contains an array.
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: [1, 2, 3], x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: [1, 2, 3], x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], x: 20},
            {_id: 3, [timeFieldName]: t[3], x: 30},
        ],
    );

    runTest(
        // Metadata field contains an object. Its ordering does not affect which bucket is used.
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: {a: 1, b: 1}, x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: {a: 1, b: 1}, x: 10},
            {_id: 2, [timeFieldName]: t[2], [metaFieldName]: {b: 1, a: 1}, x: 20},
        ],
        [{_id: 3, [timeFieldName]: t[3], [metaFieldName]: {a: 1}, x: 30}],
    );

    runTest(
        // Metadata field contains an array within an object.
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: {a: [{b: 1, c: 0}]}, x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: {a: [{c: 0, b: 1}]}, x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], [metaFieldName]: {a: [{b: 2, c: 0}]}, x: 20},
            {_id: 3, [timeFieldName]: t[3], [metaFieldName]: {a: [{c: 0, b: 2}]}, x: 30},
        ],
    );

    runTest(
        // Metadata field contains a nested array.
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: {a: [{b: 1, c: 0}, [{e: 1, f: 0}]]}, x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: {a: [{c: 0, b: 1}, [{f: 0, e: 1}]]}, x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], [metaFieldName]: {a: [[{e: 1, f: 0}], {b: 1, c: 0}]}, x: 20},
            {_id: 3, [timeFieldName]: t[3], [metaFieldName]: {a: [[{f: 0, e: 1}], {c: 0, b: 1}]}, x: 30},
        ],
    );

    runTest(
        // Metadata field contains an array.
        [
            {_id: 0, [timeFieldName]: t[0], [metaFieldName]: {a: [1, 2, 3]}, x: 0},
            {_id: 1, [timeFieldName]: t[1], [metaFieldName]: {a: [1, 2, 3]}, x: 10},
        ],
        [
            {_id: 2, [timeFieldName]: t[2], [metaFieldName]: {a: [2, 1, 3]}, x: 20},
            {_id: 3, [timeFieldName]: t[3], [metaFieldName]: {a: [2, 1, 3]}, x: 30},
        ],
    );
});
