/**
 * Tests maximum number of measurements held in each bucket in a time-series buckets collection.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   requires_collstats,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collNamePrefix = 'timeseries_bucket_limit_count_';

    // Assumes each bucket has a limit of 1000 measurements.
    const bucketMaxCount = 1000;
    const numDocs = bucketMaxCount + 100;

    const timeFieldName = 'time';

    const runTest = function(numDocsPerInsert) {
        const coll = db.getCollection(collNamePrefix + numDocsPerInsert);
        const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
        coll.drop();

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
        assert.contains(bucketsColl.getName(), db.getCollectionNames());
        if (TestData.runningWithBalancer) {
            // In suites running moveCollection in the background, it is possible to hit the issue
            // described by SERVER-89349 which will result in more bucket documents being created.
            // Creating an index on the time field allows the buckets to be reopened, allowing the
            // counts in this test to be accurate.
            assert.commandWorked(coll.createIndex({[timeFieldName]: 1}));
        }

        let docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, [timeFieldName]: ISODate("2024-01-01T01:00:00Z"), x: i});
            if ((i + 1) % numDocsPerInsert === 0) {
                assert.commandWorked(insert(coll, docs), 'failed to insert docs: ' + tojson(docs));
                docs = [];
            }
        }

        // Check view.
        const viewDocs = coll.find({}, {x: 1}).sort({_id: 1}).toArray();
        assert.eq(numDocs, viewDocs.length, viewDocs);
        for (let i = 0; i < numDocs; i++) {
            const viewDoc = viewDocs[i];
            assert.eq(i, viewDoc._id, 'unexpected _id in doc: ' + i + ': ' + tojson(viewDoc));
            assert.eq(i, viewDoc.x, 'unexpected field x in doc: ' + i + ': ' + tojson(viewDoc));
        }

        // Check bucket collection.
        const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();

        jsTestLog('Collection stats for ' + coll.getFullName() + ': ' + tojson(coll.stats()));

        if (!TestData.runningWithBalancer) {
            assert.eq(2, bucketDocs.length, tojson(bucketDocs));

            // Check both buckets.
            // First bucket should be full with 'bucketMaxCount' documents.
            assert.eq(0,
                      bucketDocs[0].control.min._id,
                      'invalid control.min for _id in first bucket: ' + tojson(bucketDocs));
            assert.eq(0,
                      bucketDocs[0].control.min.x,
                      'invalid control.min for x in first bucket: ' + tojson(bucketDocs));
            assert.eq(bucketMaxCount - 1,
                      bucketDocs[0].control.max._id,
                      'invalid control.max for _id in first bucket: ' + tojson(bucketDocs));
            assert.eq(bucketMaxCount - 1,
                      bucketDocs[0].control.max.x,
                      'invalid control.max for x in first bucket: ' + tojson(bucketDocs));
            assert(TimeseriesTest.isBucketCompressed(bucketDocs[0].control.version),
                   'unexpected control.version in first bucket: ' + tojson(bucketDocs));
            assert(!bucketDocs[0].control.hasOwnProperty("closed"),
                   'unexpected control.closed in first bucket: ' + tojson(bucketDocs));

            // Second bucket should contain the remaining documents.
            assert.eq(bucketMaxCount,
                      bucketDocs[1].control.min._id,
                      'invalid control.min for _id in second bucket: ' + tojson(bucketDocs));
            assert.eq(bucketMaxCount,
                      bucketDocs[1].control.min.x,
                      'invalid control.min for x in second bucket: ' + tojson(bucketDocs));
            assert.eq(numDocs - 1,
                      bucketDocs[1].control.max._id,
                      'invalid control.max for _id in second bucket: ' + tojson(bucketDocs));
            assert.eq(numDocs - 1,
                      bucketDocs[1].control.max.x,
                      'invalid control.max for x in second bucket: ' + tojson(bucketDocs));
            if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
                assert(TimeseriesTest.isBucketCompressed(bucketDocs[1].control.version),
                       'unexpected control.version in second bucket: ' + tojson(bucketDocs));
            } else {
                assert.eq(TimeseriesTest.BucketVersion.kUncompressed,
                          bucketDocs[1].control.version,
                          'unexpected control.version in second bucket: ' + tojson(bucketDocs));
            }
            assert(!bucketDocs[1].control.hasOwnProperty("closed"),
                   'unexpected control.closed in second bucket: ' + tojson(bucketDocs));
        } else {
            // If we are running with moveCollection in the background, we may run into the issue
            // described by SERVER-89349 which can result in more bucket documents than needed.
            // However, we still want to check that the number of documents is within the acceptable
            // range.
            assert.lte(2, bucketDocs.length, tojson(bucketDocs));
            let currMin = 0;
            bucketDocs.forEach((doc) => {
                assert.eq(currMin,
                          doc.control.min._id,
                          'invalid control.min for _id in bucket: ' + tojson(doc));
                assert.eq(currMin,
                          doc.control.min.x,
                          'invalid control.min for x in bucket: ' + tojson(doc));
                let bucketMaxId = doc.control.max._id;
                let bucketMaxX = doc.control.max.x;
                assert.lte(bucketMaxId - currMin,
                           bucketMaxCount,
                           'Too high _id range in bucket: ' + tojson(doc));
                assert.lte(bucketMaxX - currMin,
                           bucketMaxCount,
                           'Too high x range in bucket: ' + tojson(doc));
                if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
                    assert(TimeseriesTest.isBucketCompressed(doc.control.version),
                           'unexpected control.version in bucket: ' + tojson(doc));
                }
                assert(!doc.control.hasOwnProperty("closed"),
                       'unexpected control.closed in bucket: ' + tojson(doc));
                currMin = bucketMaxId + 1;
            });
        }
    };

    runTest(1);
    runTest(numDocs / 2);
    runTest(numDocs);
});
