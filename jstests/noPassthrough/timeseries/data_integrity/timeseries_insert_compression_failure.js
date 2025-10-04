/**
 * Tests that if time-series inserts fails due to bucket compression failure, those inserts can be
 * retried and will not try writing to the same corrupt bucket.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

// Disable testing diagnostics because bucket compression failure results in a tripwire assertion.
TestData.testingDiagnosticsEnabled = false;

const corruptBucket = function (db, coll, bucketId) {
    // Corrupt the bucket by adding an out-of-order index in the "a" column. This will make the
    // bucket uncompressible.
    let res = assert.commandWorked(
        getTimeseriesCollForRawOps(db, coll).updateOne(
            {_id: bucketId},
            {$set: {"data.a.0": 0, "control.min.a": 0}},
            getRawOperationSpec(db),
        ),
    );
    assert.eq(res.modifiedCount, 1);
};

const runTest = function (ordered) {
    jsTestLog("Ordered: [" + ordered.toString() + "]");
    const conn = MongoRunner.runMongod();

    const db = conn.getDB(jsTestName());
    const collName = ordered ? "ordered" : "unordered";
    const coll = db[collName];
    const timeFieldName = "t";
    const metaFieldName = "m";

    assert.commandWorked(
        db.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    const timestamps = [
        ISODate("2024-01-16T20:48:39.448Z"),
        ISODate("2024-01-16T20:48:40.448Z"),
        ISODate("2024-01-16T20:48:41.448Z"),
        ISODate("2024-01-16T20:48:42.448Z"),
        ISODate("2024-01-16T20:48:43.448Z"),
        ISODate("2024-01-16T20:48:44.448Z"),
    ];
    const bucket = {
        _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
        control: {
            version: TimeseriesTest.BucketVersion.kUncompressed,
            min: {
                _id: ObjectId("65a6eba7e6d2e848e08c3750"),
                [timeFieldName]: ISODate("2024-01-16T20:48:00Z"),
                a: 1,
            },
            max: {
                _id: ObjectId("65a6eba7e6d2e848e08c3751"),
                [timeFieldName]: timestamps[0],
                a: 1,
            },
        },
        meta: 0,
        data: {
            _id: {
                0: ObjectId("65a6eba7e6d2e848e08c3750"),
                1: ObjectId("65a6eba7e6d2e848e08c3751"),
            },
            [timeFieldName]: {
                0: timestamps[0],
                1: timestamps[0],
            },
            a: {
                1: 1,
            },
        },
    };
    assert.commandWorked(getTimeseriesCollForRawOps(db, coll).insertOne(bucket, getRawOperationSpec(db)));

    corruptBucket(db, coll, bucket._id);

    let stats = assert.commandWorked(coll.stats());
    assert.eq(1, TimeseriesTest.getStat(stats.timeseries, "bucketCount"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketInserts"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketUpdates"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsOpenedDueToMetadata"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToCount"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToSchemaChange"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToSize"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToTimeForward"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToMemoryThreshold"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numCommits"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numWaits"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numMeasurementsCommitted"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToReopening"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsArchivedDueToTimeBackward"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsReopened"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsKeptOpenDueToLargeMeasurements"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsFrozen"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numCompressedBucketsConvertedToUnsorted"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsFetched"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsQueried"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketQueriesFailed"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numDuplicateBucketsReopened"));
    TimeseriesTest.checkBucketReopeningsFailedCounters(stats.timeseries, {});

    const insert = function (docs) {
        return db.runCommand({insert: coll.getName(), documents: docs, ordered: ordered});
    };

    const docs = [
        {[timeFieldName]: timestamps[0], [metaFieldName]: 1, a: 2}, // Bucket 1
        {[timeFieldName]: timestamps[1], [metaFieldName]: 0, a: 2}, // Bucket 0 (corrupt)
        {[timeFieldName]: timestamps[2], [metaFieldName]: 2, a: 2}, // Bucket 2
        {[timeFieldName]: timestamps[3], [metaFieldName]: 1, a: 3}, // Bucket 1
        {[timeFieldName]: timestamps[4], [metaFieldName]: 0, a: 3}, // Bucket 0 (corrupt)
        {[timeFieldName]: timestamps[5], [metaFieldName]: 1, a: 4}, // Bucket 1
    ];
    assert.eq(coll.find().itcount(), 2);

    let res = insert(docs);
    assert.eq(res.ok, 1);
    assert.eq(res.n, 6);
    assert(res["writeErrors"] === undefined);

    stats = assert.commandWorked(coll.stats());

    // Bucket 0 (frozen) + Bucket 0 (new) + Bucket 1 + Bucket 2
    assert.eq(4, TimeseriesTest.getStat(stats.timeseries, "bucketCount"));
    assert.eq(3, TimeseriesTest.getStat(stats.timeseries, "numBucketInserts"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketUpdates"));
    assert.eq(2, TimeseriesTest.getStat(stats.timeseries, "numBucketsOpenedDueToMetadata"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToCount"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToSchemaChange"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToSize"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToTimeForward"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToMemoryThreshold"));
    assert.eq(3, TimeseriesTest.getStat(stats.timeseries, "numCommits"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numWaits"));
    assert.eq(6, TimeseriesTest.getStat(stats.timeseries, "numMeasurementsCommitted"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToReopening"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsArchivedDueToTimeBackward"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsReopened"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsKeptOpenDueToLargeMeasurements"));
    assert.eq(1, TimeseriesTest.getStat(stats.timeseries, "numBucketsFrozen"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numCompressedBucketsConvertedToUnsorted"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsFetched"));
    assert.eq(1, TimeseriesTest.getStat(stats.timeseries, "numBucketsQueried"));
    assert.eq(2, TimeseriesTest.getStat(stats.timeseries, "numBucketQueriesFailed"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numDuplicateBucketsReopened"));
    TimeseriesTest.checkBucketReopeningsFailedCounters(stats.timeseries, {});
    assert.eq(coll.find().itcount(), 8);

    res = assert.commandWorked(insert(docs.slice(1)));
    assert.eq(res.n, 5);
    assert(res["writeErrors"] === undefined);

    stats = assert.commandWorked(coll.stats());
    assert.eq(4, TimeseriesTest.getStat(stats.timeseries, "bucketCount"));
    assert.eq(3, TimeseriesTest.getStat(stats.timeseries, "numBucketInserts"));
    assert.eq(3, TimeseriesTest.getStat(stats.timeseries, "numBucketUpdates"));
    assert.eq(2, TimeseriesTest.getStat(stats.timeseries, "numBucketsOpenedDueToMetadata"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToCount"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToSchemaChange"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToSize"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToTimeForward"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToMemoryThreshold"));
    assert.eq(6, TimeseriesTest.getStat(stats.timeseries, "numCommits"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numWaits"));
    assert.eq(11, TimeseriesTest.getStat(stats.timeseries, "numMeasurementsCommitted"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsClosedDueToReopening"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsArchivedDueToTimeBackward"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsReopened"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsKeptOpenDueToLargeMeasurements"));
    assert.eq(1, TimeseriesTest.getStat(stats.timeseries, "numBucketsFrozen"));
    assert.eq(2, TimeseriesTest.getStat(stats.timeseries, "numCompressedBucketsConvertedToUnsorted"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numBucketsFetched"));
    assert.eq(1, TimeseriesTest.getStat(stats.timeseries, "numBucketsQueried"));
    assert.eq(2, TimeseriesTest.getStat(stats.timeseries, "numBucketQueriesFailed"));
    assert.eq(0, TimeseriesTest.getStat(stats.timeseries, "numDuplicateBucketsReopened"));
    TimeseriesTest.checkBucketReopeningsFailedCounters(stats.timeseries, {});

    assert.eq(coll.find().itcount(), 13);
    assert.eq(coll.find({[metaFieldName]: 0}).itcount(), 6);
    assert.eq(coll.find({[metaFieldName]: 1}).itcount(), 5);
    assert.eq(coll.find({[metaFieldName]: 2}).itcount(), 2);

    const buckets = getTimeseriesCollForRawOps(db, coll)
        .find({meta: bucket.meta})
        .rawData()
        .sort({"control.version": 1})
        .toArray();
    assert.eq(buckets.length, 2);
    assert.eq(buckets[0].control.version, TimeseriesTest.BucketVersion.kUncompressed);
    assert(buckets[0].data.t.hasOwnProperty("0"));
    assert(buckets[0].data.t.hasOwnProperty("1"));
    assert(!buckets[0].data.t.hasOwnProperty("2"));
    assert.eq(buckets[0].control.version, TimeseriesTest.BucketVersion.kUncompressed);
    assert(TimeseriesTest.isBucketCompressed(buckets[1].control.version));
    assert.eq(buckets[1].control.count, 4);

    // Check logs for bucket corruption log message.
    checkLog.containsJson(db, 8654201, {
        bucketId: function (bucketId) {
            return bucketId["$oid"] === bucket._id.valueOf();
        },
    });

    // Skip validation due to the corrupt buckets.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
};

runTest(true);
runTest(false);
