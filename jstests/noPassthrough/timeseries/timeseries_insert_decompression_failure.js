/**
 * Tests that time-series inserts get written into a new bucket if decompression of an existing
 * bucket fails.
 *
 * @tags: [
 *   featureFlagTimeseriesAlwaysUseCompressedBuckets,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
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
            version: NumberInt(2),
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
            count: NumberInt(2),
        },
        meta: 0,
        data: {
            t: BinData(7, "CQAYhggUjQEAAIAOAAAAAAAAAAA="),
            a: BinData(7, "AQAAAAAAAAAAAJAuAAAAAAAAAAA="),
            _id: BinData(7, "BwBlpuun5tLoSOCMN1CALgAAAAAAAAAA"),
        }
    };
    assert.commandWorked(bucketsColl.insert(bucket));

    // Corrupt the compressed "a" column so that the bucket cannot be decompressd.
    const res = assert.commandWorked(bucketsColl.updateOne({_id: bucket._id}, {
        $set: {
            "data.a": BinData(
                7, bucket.data.a.base64().substr(0, 11) + "B" + bucket.data.a.base64().substr(12))
        }
    }));
    assert.eq(res.modifiedCount, 1);

    assert.commandWorked(insert(coll, [
        {t: time, m: 1, a: 2},  // Bucket 1
        {t: time, m: 0, a: 2},  // Bucket 0 (corrupt)
        {t: time, m: 2, a: 2},  // Bucket 2
        {t: time, m: 1, a: 3},  // Bucket 1
        {t: time, m: 0, a: 3},  // Bucket 0 (corrupt)
        {t: time, m: 1, a: 4},  // Bucket 1
    ]));
    assert.eq(coll.find({m: 1}).itcount(), 3);
    assert.eq(coll.find({m: 2}).itcount(), 1);

    const buckets = bucketsColl.find({meta: bucket.meta}).toArray();
    assert.eq(buckets.length, 2);
    assert(TimeseriesTest.isBucketCompressed(buckets[0].control.version));
    assert.eq(buckets[0].control.count, 2);
    assert(TimeseriesTest.isBucketCompressed(buckets[1].control.version));
    assert.eq(buckets[1].control.count, 2);

    // Prevent validation from running on this collection and throwing an error. As of SERVER-86451
    // timeseries validation inconsistencies error instead of returning warnings when testing, and
    // this test would fail on shutdown validation because of the corrupt bucket we created.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
});
