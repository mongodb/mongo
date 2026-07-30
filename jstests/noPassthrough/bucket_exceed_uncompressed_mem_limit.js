/**
 * Tests that a raw bucket insert is rejected with ExceededMemoryLimit when the BSONColumn data
 * expands beyond the configured bsonMaxExpandedMemUsage limit.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const bucketMaxSize = 1024 * 1024 * 16; //  16 MB
const bucketMinMaxElemCount = 1000;
const lowMemLimit = 65536; // 64 KB

describe("bucket exceeding uncompressed mem limit is rejected", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet({
            setParameter: {
                // FTDC samples can exceed the intentionally low BSON memory limit used below.
                diagnosticDataCollectionEnabled: false,
                timeseriesBucketMaxSize: bucketMaxSize,
                timeseriesBucketMinCount: bucketMinMaxElemCount,
                timeseriesBucketMaxCount: bucketMinMaxElemCount,
            },
        });
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        this.originalMemLimit = assert.commandWorked(
            this.primary.adminCommand({getParameter: 1, bsonMaxExpandedMemUsage: 1}),
        ).bsonMaxExpandedMemUsage;
        this.testDB = this.primary.getDB("test").getSiblingDB(jsTestName());
        this.timeField = "t";
        this.metaField = "m";
    });

    after(function () {
        if (this.primary) {
            assert.commandWorked(
                this.primary.adminCommand({
                    setParameter: 1,
                    bsonMaxExpandedMemUsage: this.originalMemLimit,
                }),
            );
        }
        if (this.rst) {
            this.rst.stopSet();
        }
    });

    it("rejects raw bucket insert with ExceededMemoryLimit", function () {
        const {testDB, primary, timeField, metaField} = this;
        const coll = testDB.coll;

        // Create a timeseries collection and insert measurements to generate a compressed bucket
        // whose BSONColumn data expands to more than 64KB but well under 200MB.
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {
                timeseries: {timeField: timeField, metaField: metaField},
            }),
        );

        const baseTime = ISODate("2022-08-31T00:00:00.000Z");
        const largeStr = "x".repeat(200);
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 500; i++) {
            bulk.insert({
                [timeField]: new Date(baseTime.getTime() + i * 1000),
                [metaField]: "a",
                x: {a: largeStr, b: NumberInt(3)},
            });
        }
        assert.commandWorked(bulk.execute());

        // Read the raw bucket back and sanity-check that it contains data for the time field.
        // When the bucket is compressed, also verify it uses BSONColumn (BinData subtype 7).
        const bucketsColl = getTimeseriesCollForRawOps(testDB, testDB[coll.getName()]);
        const rawBuckets = bucketsColl
            .aggregate([{$match: {}}], getRawOperationSpec(testDB))
            .toArray();
        assert.gt(rawBuckets.length, 0, "Expected at least one bucket");
        const bucketObj = rawBuckets[0];
        assert(
            bucketObj.data && bucketObj.data[timeField],
            "Expected bucket to contain data for the time field",
        );
        if (typeof bucketObj.data[timeField].subtype === "function") {
            assert.eq(
                bucketObj.data[timeField].subtype(),
                7,
                "Expected BSONColumn (BinData subtype 7) data for the time field",
            );
        }

        // Drop and lower the memory limit.
        assert(coll.drop());
        assert.commandWorked(
            primary.adminCommand({setParameter: 1, bsonMaxExpandedMemUsage: lowMemLimit}),
        );

        // Re-create the collection and try to insert the bucket. The server should reject it
        // with ExceededMemoryLimit because the BSONColumn expansion exceeds the lowered limit.
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {
                timeseries: {timeField: timeField, metaField: metaField},
            }),
        );

        assert.throwsWithCode(
            () =>
                getTimeseriesCollForRawOps(testDB, testDB[coll.getName()]).insertOne(
                    bucketObj,
                    getRawOperationSpec(testDB),
                ),
            ErrorCodes.ExceededMemoryLimit,
        );

        assert(coll.drop());
    });
});
