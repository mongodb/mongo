/**
 * Tests that an uncompressed time-series bucket gets compressed upon being written to.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const time = ISODate("2024-01-16T20:48:39.448Z");

let count = 0;

const runTest = function(write, numExpectedDocs) {
    const coll = db["coll_" + count++];
    const bucketsColl = db["system.buckets." + coll.getName()];

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    const uncompressedBucket = {
        _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
        control: {
            version: 1,
            min: {
                _id: ObjectId("65a6eba7e6d2e848e08c3750"),
                t: ISODate("2024-01-16T20:48:00Z"),
                a: 0,
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
                0: 0,
                1: 1,
            },
        }
    };

    assert.commandWorked(bucketsColl.insert(uncompressedBucket));
    assert.eq(coll.find().itcount(), 2);
    assert.eq(bucketsColl.find().itcount(), 1);
    assert.eq(bucketsColl.find().toArray()[0].control.version,
              TimeseriesTest.BucketVersion.kUncompressed);

    assert.commandWorked(write(coll));
    assert.eq(coll.find().itcount(), numExpectedDocs);
    assert.eq(bucketsColl.find().itcount(), 1);
    assert(TimeseriesTest.isBucketCompressed(bucketsColl.find().toArray()[0].control.version));
};

runTest((coll) => coll.insert({t: time, m: 0, a: 2}, {ordered: false}), 3);
runTest((coll) => coll.insert({t: time, m: 0, a: 2}, {ordered: true}), 3);
// TODO (SERVER-68058): Remove this condition.
if (FeatureFlagUtil.isPresentAndEnabled(db, "TimeseriesUpdatesSupport")) {
    runTest((coll) => coll.update({}, {$set: {m: 1}}, {multi: true}), 2);
    runTest((coll) => coll.update({}, {$set: {a: 2}}, {multi: true}), 2);
}
runTest((coll) => coll.deleteOne({a: 0}), 1);

MongoRunner.stopMongod(conn);
