/**
 * Tests that an uncompressed time-series bucket gets compressed upon being reopened.
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// Disable testing diagnostics because bucket compression failure results in a tripwire assertion.
TestData.testingDiagnosticsEnabled = false;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const metaField = 'm';
const timeField = 't';

const time1_first = ISODate("2024-01-16T20:48:39.448Z");
const time1_later = ISODate("2024-01-16T20:48:50.448Z");
const time2_first = ISODate("2024-02-29T07:55:50.212Z");

let collCount = 0;

const checkAllBucketsCompressed = function(coll) {
    jsTestLog("Confirm all buckets are compressed.");
    var docs = coll.find().toArray();
    for (let i = 0; i < docs.length; ++i) {
        jsTestLog(docs[i]);
        assert(TimeseriesTest.isBucketCompressed(docs[i].control.version));
    }
};

const runTest = function(isCorrupted = false) {
    jsTestLog("runTest(isCorrupted: [" + isCorrupted.toString() + "])");
    const collName = "coll_" + collCount++;
    const coll = db[collName];
    const bucketsColl = db["system.buckets." + coll.getName()];

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    const uncompressedBucket = {
        _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
        control: {
            version: 1,
            min: {
                _id: ObjectId("65a6eba7e6d2e848e08c3750"),
                [timeField]: ISODate("2024-01-16T20:48:00.000Z"),
                a: 0,
            },
            max: {
                _id: ObjectId("65a6eba7e6d2e848e08c3751"),
                [timeField]: time1_first,
                a: 1,
            },
        },
        meta: 0,
        data: {
            _id: {
                0: ObjectId("65a6eba7e6d2e848e08c3750"),
                1: ObjectId("65a6eba7e6d2e848e08c3751"),
            },
            [timeField]: {
                0: time1_first,
                1: time1_first,
            },
            a: {
                0: 0,
                1: 1,
            },
        }
    };

    jsTestLog("Insert uncompressed bucket document.");
    assert.commandWorked(bucketsColl.insert(uncompressedBucket));
    assert.eq(coll.find().itcount(), 2);
    assert.eq(bucketsColl.find().itcount(), 1);
    assert.eq(bucketsColl.find().toArray()[0].control.version,
              TimeseriesTest.BucketVersion.kUncompressed);

    if (isCorrupted) {
        jsTestLog("Corrupting the bucket by adding an extra data field.");
        // Corrupt the uncompressed bucket by adding an extra data field to it. This
        // will make the bucket uncompressible.
        let res = assert.commandWorked(
            bucketsColl.updateOne({_id: uncompressedBucket._id}, {$set: {"data.a.3": 6}}));
        jsTestLog(bucketsColl.find().toArray());
        assert.eq(res.modifiedCount, 1);
    }

    jsTestLog("Reopen uncompressed bucket with a new measurement that should land in it.");
    assert.commandWorked(coll.insert({[timeField]: time1_later, [metaField]: 0, a: 2}));

    const stats = assert.commandWorked(coll.stats());
    jsTestLog(stats.timeseries);
    if (isCorrupted) {
        assert.eq(0, stats.timeseries['numBucketsReopened']);
        assert.eq(0, stats.timeseries['numBucketUpdates']);
        assert.eq(1, stats.timeseries['numBucketsFrozen']);
    } else {
        assert.eq(1, stats.timeseries['numBucketsReopened']);
        assert.eq(1, stats.timeseries['numBucketUpdates']);
        assert.eq(0, stats.timeseries['numBucketsFrozen']);
    }
    assert.eq(1, stats.timeseries['numCommits']);

    if (isCorrupted) {
        assert.eq(bucketsColl.find().itcount(), 2);
        jsTestLog(
            "Remove corrupted bucket to prevent the validate post-hook from seeing it after the test.");
        assert.commandWorked(bucketsColl.remove({_id: ObjectId("65a6eb806ffc9fa4280ecac4")}));
    } else {
        assert.eq(bucketsColl.find().itcount(), 1);
    }

    checkAllBucketsCompressed(bucketsColl);
};

runTest();
runTest(/*isCorrupted=*/ true);

MongoRunner.stopMongod(conn);
