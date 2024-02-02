/**
 * Tests that time-series inserts get retried with a different bucket if compression fails.
 *
 * @tags: [
 *   featureFlagTimeseriesAlwaysUseCompressedBuckets,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

// Disable testing diagnostics because bucket compression failure results in a tripwire assertion.
TestData.testingDiagnosticsEnabled = false;

// TODO (SERVER-86072): Run test with both ordered and unordered inserts.
// TimeseriesTest.run((insert) => {
const insert = function(coll, docs) {
    return coll.insert(docs);
};
{
    const conn = MongoRunner.runMongod();

    const db = conn.getDB(jsTestName());
    const coll = db.coll;
    const bucketsColl = db.system.buckets.coll;

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    const time = ISODate("2024-01-16T20:48:39.448Z");
    const bucket = {
        _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
        control: {
            version: NumberInt(1),
            min: {
                _id: ObjectId("65a6eba7e6d2e848e08c3750"),
                t: ISODate("2024-01-16T20:48:00Z"),
                a: 1,
            },
            max: {
                _id: ObjectId("65a6eba7e6d2e848e08c3751"),
                t: time,
                a: 1,
            },
        },
        meta: 0,
        data: {
            _id: {
                0: ObjectId("65a6eba7e6d2e848e08c3750"),
                1: ObjectId("65a6eba7e6d2e848e08c3751"),
            },
            t: {
                0: time,
                1: time,
            },
            a: {
                1: 1,
            },
        }
    };
    assert.commandWorked(bucketsColl.insert(bucket));

    // Corrupt the bucket by adding an out-of-order index in the "a" column. This will make the
    // bucket uncompressable.
    const res = assert.commandWorked(
        bucketsColl.updateOne({_id: bucket._id}, {$set: {"data.a.0": 0, "control.min.a": 0}}));
    assert.eq(res.modifiedCount, 1);

    assert.commandWorked(insert(coll, [
        {t: time, m: 1, a: 2},  // Bucket 1
        {t: time, m: 0, a: 2},  // Bucket 0 (corrupt)
        {t: time, m: 2, a: 2},  // Bucket 2
        {t: time, m: 1, a: 3},  // Bucket 1
        {t: time, m: 0, a: 3},  // Bucket 0 (corrupt)
        {t: time, m: 1, a: 4},  // Bucket 1
    ]));
    assert.eq(coll.find().itcount(), 8);
    assert.eq(coll.find({m: 0}).itcount(), 4);
    assert.eq(coll.find({m: 1}).itcount(), 3);
    assert.eq(coll.find({m: 2}).itcount(), 1);

    const buckets = bucketsColl.find({meta: bucket.meta}).sort({"control.version": 1}).toArray();
    assert.eq(buckets.length, 2);
    assert.eq(buckets[0].control.version, TimeseriesTest.BucketVersion.kUncompressed);
    assert(buckets[0].data.t.hasOwnProperty("0"));
    assert(buckets[0].data.t.hasOwnProperty("1"));
    assert(!buckets[0].data.t.hasOwnProperty("2"));
    assert.eq(buckets[1].control.version, TimeseriesTest.BucketVersion.kCompressed);
    assert.eq(buckets[1].control.count, 2);

    checkLog.containsJson(db, 8065300, {
        bucketId: function(bucketId) {
            return bucketId["$oid"] === bucket._id.valueOf();
        }
    });

    // Skip validation due to the corrupt buckets.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
}
//});
