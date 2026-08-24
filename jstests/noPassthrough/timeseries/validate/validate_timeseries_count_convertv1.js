/**
 * Tests reopen behavior when encountering a v1 bucket that needs ugprade before an insert.
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const collNamePrefix = jsTestName();
const timeFieldName = "timestamp";
const metaFieldName = "metadata";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

// Manually crafting buckets in tests is heavily discouraged due to fragility, but necessary here.
const createVersion1Bucket = function (count) {
    const minTime = ISODate("2024-01-01T00:00:00Z");
    const maxTime = ISODate("2024-01-01T00:01:00Z");
    const bucketIdPrefix = Math.floor(minTime.getTime() / 1000)
        .toString(16)
        .padStart(8, "0");
    return {
        _id: ObjectId(bucketIdPrefix + "0102030405060708"),
        control: {
            version: TimeseriesTest.BucketVersion.kUncompressed,
            min: {timestamp: minTime, temp: 5},
            max: {timestamp: maxTime, temp: 500},
            "count": count,
        },
        meta: {"sensorId": 1, "type": "temperature"},
        data: {
            timestamp: {"0": minTime, "1": maxTime},
            temp: {"0": 5, "1": 500},
        },
    };
};

// v2 buckets should always be created, but testing an unsorted insert that gets sorted on upgrade.
const runTest = function (description, correctExtraCount, targetBucketVersion) {
    let collName =
        collNamePrefix +
        "__" +
        description +
        "_v" +
        targetBucketVersion +
        (correctExtraCount ? "correct" : "incorrect");
    jsTestLog("Test: " + collName);
    db.getCollection(collName).drop();
    assert.commandWorked(
        db.createCollection(collName, {
            timeseries: {timeField: timeFieldName, metaField: metaFieldName, granularity: "hours"},
        }),
    );
    let coll = db.getCollection(collName);

    jsTestLog("Turn validator off to allow bad bucket.");
    assert.commandWorked(
        conn
            .getDB("admin")
            .runCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: true}),
    );

    let corruptBucketFilter;
    if (correctExtraCount) {
        jsTestLog(
            "Insert a v1 bucket that illegally contains an otherwise 'correct' control.count field.",
        );
        const v1Bucket = createVersion1Bucket(2);
        assert.commandWorked(
            getTimeseriesCollForRawOps(db, coll).insertOne(v1Bucket, getRawOperationSpec(db)),
        );
        corruptBucketFilter = {_id: v1Bucket._id};
    } else {
        jsTestLog("Insert a v1 bucket that illegally contains an incorrect control.count field.");
        const v1Bucket = createVersion1Bucket(999);
        assert.commandWorked(
            getTimeseriesCollForRawOps(db, coll).insertOne(v1Bucket, getRawOperationSpec(db)),
        );
        corruptBucketFilter = {_id: v1Bucket._id};
    }

    jsTestLog("Turn validator back on.");
    assert.commandWorked(
        conn
            .getDB("admin")
            .runCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: false}),
    );

    if (description == "open_bucket") {
        jsTestLog("Upgrade the open bucket to compressed format");
        if (targetBucketVersion == TimeseriesTest.BucketVersion.kCompressedSorted) {
            coll.insertOne({
                [metaFieldName]: {"sensorId": 1, "type": "temperature"},
                [timeFieldName]: ISODate("2024-01-01T00:02:00Z"),
                "temp": 100,
            });
        } else if (targetBucketVersion == TimeseriesTest.BucketVersion.kCompressedUnsorted) {
            coll.insertOne({
                [metaFieldName]: {"sensorId": 1, "type": "temperature"},
                [timeFieldName]: ISODate("2024-01-01T00:00:30Z"),
                "temp": 100,
            });
        }
    } else if (description == "reopen_closed_bucket") {
        jsTestLog("Close bucket");
        coll.insertOne({
            [metaFieldName]: {"sensorId": 1, "type": "temperature"},
            [timeFieldName]: ISODate("2026-07-23T00:00:00Z"),
            "temp": 100,
        });

        jsTestLog("Target a measurement to closed bucket");
        if (targetBucketVersion == TimeseriesTest.BucketVersion.kCompressedSorted) {
            coll.insertOne({
                [metaFieldName]: {"sensorId": 1, "type": "temperature"},
                [timeFieldName]: ISODate("2024-01-01T00:02:00Z"),
                "temp": 100,
            });
        } else if (targetBucketVersion == TimeseriesTest.BucketVersion.kCompressedUnsorted) {
            coll.insertOne({
                [metaFieldName]: {"sensorId": 1, "type": "temperature"},
                [timeFieldName]: ISODate("2024-01-01T00:00:30Z"),
                "temp": 100,
            });
        }
    }

    const extraBuckets = description == "reopen_closed_bucket" ? 1 : 0;

    let res = coll.validate();
    let numBuckets = db[collName].find().rawData().itcount();
    let numDocs = db[collName].find().itcount();

    let controlCount = getTimeseriesCollForRawOps(db, coll).findOneWithRawData(
        corruptBucketFilter,
        getRawOperationSpec(db),
    ).control.count;

    assert.eq(controlCount, 3);
    assert.eq(numDocs, 3 + extraBuckets);
    assert.eq(numBuckets, 1 + extraBuckets);
    assert.eq(res.errors.length, 0);
    assert.eq(coll.stats().timeseries.numBucketsFrozen, 0);
    assert(res.valid);
};

runTest("open_bucket", true, TimeseriesTest.BucketVersion.kCompressedSorted);
runTest("open_bucket", false, TimeseriesTest.BucketVersion.kCompressedSorted);
runTest("open_bucket", true, TimeseriesTest.BucketVersion.kCompressedUnsorted);
runTest("open_bucket", false, TimeseriesTest.BucketVersion.kCompressedUnsorted);
runTest("reopen_closed_bucket", true, TimeseriesTest.BucketVersion.kCompressedSorted);
runTest("reopen_closed_bucket", false, TimeseriesTest.BucketVersion.kCompressedSorted);
runTest("reopen_closed_bucket", true, TimeseriesTest.BucketVersion.kCompressedUnsorted);
runTest("reopen_closed_bucket", false, TimeseriesTest.BucketVersion.kCompressedUnsorted);

MongoRunner.stopMongod(conn, null, {skipValidation: true});
