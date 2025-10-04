/**
 * Tests maximum size of measurements held in each bucket of a time-series collection.
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_61,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod({setParameter: {timeseriesBucketMinCount: 1}});

const dbName = jsTestName();
const db = conn.getDB(dbName);

TimeseriesTest.run((insert) => {
    const collNamePrefix = jsTestName() + "_";

    const timeFieldName = "time";

    // Assumes each bucket has a limit of 125kB on the measurements stored in the 'data' field.
    const bucketMaxSizeKB = 125;
    const numDocs = 3;

    // The measurement data should not take up all of the 'bucketMaxSizeKB' limit because we need to
    // leave room for the control.min and control.max summaries (two measurements worth of data). We
    // need to fit two measurements within this limit to trigger compression if enabled.
    const largeValueSize = ((bucketMaxSizeKB - 1) / 4) * 1024;

    const runTest = function (numDocsPerInsert) {
        const coll = db.getCollection(collNamePrefix + numDocsPerInsert);
        coll.drop();

        assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

        let docs = [];
        for (let i = 0; i < numDocs; i++) {
            // Strings greater than 16 bytes are not compressed unless they are equal to the
            // previous.
            const value = i % 2 == 0 ? "a" : "b";
            docs.push({_id: i, [timeFieldName]: ISODate(), x: value.repeat(largeValueSize)});
            if ((i + 1) % numDocsPerInsert === 0) {
                assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
                docs = [];
            }
        }

        // Check measurements.
        const userDocs = coll.find({}, {x: 1}).sort({_id: 1}).toArray();
        assert.eq(numDocs, userDocs.length, userDocs);
        for (let i = 0; i < numDocs; i++) {
            const userDoc = userDocs[i];
            assert.eq(i, userDoc._id, "unexpected _id in doc: " + i + ": " + tojson(userDoc));

            const value = i % 2 == 0 ? "a" : "b";
            assert.eq(
                value.repeat(largeValueSize),
                userDoc.x,
                "unexpected field x in doc: " + i + ": " + tojson(userDoc),
            );
        }

        const bucketDocs = getTimeseriesCollForRawOps(db, coll).find().rawData().sort({"control.min._id": 1}).toArray();
        assert.eq(2, bucketDocs.length, bucketDocs);

        // Check both buckets.
        // First bucket should be full with two documents since we spill the third document over
        // into the second bucket due to size constraints on 'data'.
        assert.eq(
            0,
            bucketDocs[0].control.min._id,
            "invalid control.min for _id in first bucket: " + tojson(bucketDocs[0].control),
        );
        assert.eq(
            "a".repeat(largeValueSize),
            bucketDocs[0].control.min.x,
            "invalid control.min for x in first bucket: " + tojson(bucketDocs[0].control),
        );
        assert.eq(
            1,
            bucketDocs[0].control.max._id,
            "invalid control.max for _id in first bucket: " + tojson(bucketDocs[0].control),
        );
        assert.eq(
            "b".repeat(largeValueSize),
            bucketDocs[0].control.max.x,
            "invalid control.max for x in first bucket: " + tojson(bucketDocs[0].control),
        );
        assert(
            TimeseriesTest.isBucketCompressed(bucketDocs[0].control.version),
            "expected first bucket to be compressed: " + tojson(bucketDocs),
        );
        assert(
            !bucketDocs[0].control.hasOwnProperty("closed"),
            "unexpected control.closed in first bucket: " + tojson(bucketDocs),
        );

        // Second bucket should contain the remaining document.
        assert.eq(
            numDocs - 1,
            bucketDocs[1].control.min._id,
            "invalid control.min for _id in second bucket: " + tojson(bucketDocs[1].control),
        );
        assert.eq(
            "a".repeat(largeValueSize),
            bucketDocs[1].control.min.x,
            "invalid control.min for x in second bucket: " + tojson(bucketDocs[1].control),
        );
        assert.eq(
            numDocs - 1,
            bucketDocs[1].control.max._id,
            "invalid control.max for _id in second bucket: " + tojson(bucketDocs[1].control),
        );
        assert.eq(
            "a".repeat(largeValueSize),
            bucketDocs[1].control.max.x,
            "invalid control.max for x in second bucket: " + tojson(bucketDocs[1].control),
        );
        assert.eq(
            TimeseriesTest.BucketVersion.kCompressedSorted,
            bucketDocs[1].control.version,
            "unexpected control.version in second bucket: " + tojson(bucketDocs),
        );

        assert(
            !bucketDocs[1].control.hasOwnProperty("closed"),
            "unexpected control.closed in second bucket: " + tojson(bucketDocs),
        );
    };

    runTest(1);
    runTest(numDocs);
});

MongoRunner.stopMongod(conn);
