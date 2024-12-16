/**
 * Tests maximum time-range of measurements held in each bucket in a time-series buckets collection.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns
 *   # may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # We assume that all nodes in a mixed-mode replica set are using compressed inserts to a
 *   # time-series collection.
 *   requires_fcv_71,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collNamePrefix = jsTestName() + '_';

    const timeFieldName = 'time';

    // Assumes the measurements in each bucket span at most one hour (based on the time field).
    // Make sure we have three measurements to trigger compression if enabled. The data types in
    // this test are so small so two measurements may not yield a smaller compressed object
    const docTimes = [
        ISODate("2020-11-13T01:00:00Z"),
        ISODate("2020-11-13T01:00:01Z"),
        ISODate("2020-11-13T01:00:02Z"),
        ISODate("2020-11-13T03:00:00Z")
    ];
    const numDocs = 4;

    const runTest = function(numDocsPerInsert) {
        const coll = db.getCollection(collNamePrefix + numDocsPerInsert);
        const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
        coll.drop();

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
        assert.contains(bucketsColl.getName(), db.getCollectionNames());

        let docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, [timeFieldName]: docTimes[i], x: i});
            if ((i + 1) % numDocsPerInsert === 0) {
                assert.commandWorked(insert(coll, docs), 'failed to insert docs: ' + tojson(docs));
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
        const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();

        // Check both buckets.
        // First bucket should be not contain both documents because the time of the second
        // measurement is ahead of the first document by more than 'bucketMaxTimeRangeHours'.
        if (!TestData.runningWithBalancer) {
            assert.eq(2, bucketDocs.length, bucketDocs);
            assert.eq(
                0,
                bucketDocs[0].control.min._id,
                'invalid control.min for _id in first bucket: ' + tojson(bucketDocs[0].control));
            assert.eq(
                0,
                bucketDocs[0].control.min.x,
                'invalid control.min for x in first bucket: ' + tojson(bucketDocs[0].control));
            assert.eq(
                docTimes[0],
                bucketDocs[0].control.min[timeFieldName],
                'invalid control.min for time in first bucket: ' + tojson(bucketDocs[0].control));
            assert.eq(
                2,
                bucketDocs[0].control.max._id,
                'invalid control.max for _id in first bucket: ' + tojson(bucketDocs[0].control));
            assert.eq(
                2,
                bucketDocs[0].control.max.x,
                'invalid control.max for x in first bucket: ' + tojson(bucketDocs[0].control));
            assert.eq(
                docTimes[2],
                bucketDocs[0].control.max[timeFieldName],
                'invalid control.max for time in first bucket: ' + tojson(bucketDocs[0].control));
            assert(TimeseriesTest.isBucketCompressed(bucketDocs[0].control.version),
                   'unexpected control.version in first bucket: ' + tojson(bucketDocs));

            // Second bucket should contain the remaining document.
            assert.eq(
                numDocs - 1,
                bucketDocs[1].control.min._id,
                'invalid control.min for _id in second bucket: ' + tojson(bucketDocs[1].control));
            assert.eq(
                numDocs - 1,
                bucketDocs[1].control.min.x,
                'invalid control.min for x in second bucket: ' + tojson(bucketDocs[1].control));
            assert.eq(
                docTimes[numDocs - 1],
                bucketDocs[1].control.min[timeFieldName],
                'invalid control.min for time in second bucket: ' + tojson(bucketDocs[1].control));
            assert.eq(
                numDocs - 1,
                bucketDocs[1].control.max._id,
                'invalid control.max for _id in second bucket: ' + tojson(bucketDocs[1].control));
            assert.eq(
                numDocs - 1,
                bucketDocs[1].control.max.x,
                'invalid control.max for x in second bucket: ' + tojson(bucketDocs[1].control));
            assert.eq(
                docTimes[numDocs - 1],
                bucketDocs[1].control.max[timeFieldName],
                'invalid control.max for time in second bucket: ' + tojson(bucketDocs[1].control));
            assert(TimeseriesTest.isBucketCompressed(bucketDocs[1].control.version),
                   'unexpected control.version in second bucket: ' + tojson(bucketDocs));
        } else {
            // In suites running moveCollection in the background, it is possible to hit the issue
            // described by SERVER-89349 which will result in more bucket documents being created.
            // However, we should still guarantee the time limit range for buckets.
            assert.lte(2, bucketDocs.length, bucketDocs);
            bucketDocs.forEach((bucketDoc) => {
                let bucketRangeMillis =
                    bucketDoc.control.max[timeFieldName] - bucketDoc.control.min[timeFieldName];
                assert.gte(1000 * 60 * 60, bucketRangeMillis);
                assert(TimeseriesTest.isBucketCompressed(bucketDoc.control.version),
                       'unexpected control.version in second bucket: ' + tojson(bucketDocs));
            });
        }
    };

    runTest(1);
    runTest(numDocs);
});
