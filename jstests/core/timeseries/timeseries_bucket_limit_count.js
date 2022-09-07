/**
 * Tests maximum number of measurements held in each bucket in a time-series buckets collection.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   requires_collstats,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

TimeseriesTest.run((insert) => {
    const isTimeseriesBucketCompressionEnabled =
        TimeseriesTest.timeseriesBucketCompressionEnabled(db);
    const areTimeseriesScalabilityImprovementsEnabled =
        TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db);

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

        let docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, [timeFieldName]: ISODate(), x: i});
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
        assert.eq(2, bucketDocs.length, tojson(bucketDocs));

        jsTestLog('Collection stats for ' + coll.getFullName() + ': ' + tojson(coll.stats()));

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
        assert.eq(isTimeseriesBucketCompressionEnabled ? 2 : 1,
                  bucketDocs[0].control.version,
                  'unexpected control.version in first bucket: ' + tojson(bucketDocs));

        if (areTimeseriesScalabilityImprovementsEnabled) {
            assert.eq(true,
                      bucketDocs[0].control.closed,
                      'unexpected control.closed in first bucket: ' + tojson(bucketDocs));
        } else {
            assert(!bucketDocs[0].control.hasOwnProperty("closed"),
                   'unexpected control.closed in first bucket: ' + tojson(bucketDocs));
        }

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
        assert.eq(1,
                  bucketDocs[1].control.version,
                  'unexpected control.version in second bucket: ' + tojson(bucketDocs));

        if (areTimeseriesScalabilityImprovementsEnabled) {
            assert.eq(false,
                      bucketDocs[1].control.closed,
                      'unexpected control.closed in second bucket: ' + tojson(bucketDocs));
        } else {
            assert(!bucketDocs[1].control.hasOwnProperty("closed"),
                   'unexpected control.closed in second bucket: ' + tojson(bucketDocs));
        }
    };

    runTest(1);
    runTest(numDocs / 2);
    runTest(numDocs);
});
})();
