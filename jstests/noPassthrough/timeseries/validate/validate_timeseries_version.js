/**
 * Tests that the validate command checks data consistencies of the version field in time-series
 * collections and return warnings properly.
 *
 * Version 1 indicates the bucket is uncompressed, and version 2 indicates the bucket is compressed.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {describe, before, after, afterEach, it, beforeEach} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const dbName = "test";

describe("Validate Timeseries version Tests", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(dbName);
        this.db.getCollection(collName).drop();
    });

    beforeEach(function () {
        assert.commandWorked(
            this.db.createCollection(collName, {
                timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
            }),
        );
        this.coll = this.db.getCollection(collName);
    });

    it("Inserts documents into a bucket. Checks no issues are found.", function () {
        const res = this.coll.validate();
        assert(res.valid, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 0, res);
        assert.eq(res.warnings.length, 0, res);
    });

    it("Validates changing control.version from 2 to 1 and detects the error.", function () {
        this.coll.insertMany(
            [...Array(10).keys()].map((i) => ({
                "metadata": {"sensorId": 2, "type": "temperature"},
                "timestamp": ISODate(),
                "temp": i,
            })),
            {ordered: false},
        );
        assert.eq(
            1,
            getTimeseriesCollForRawOps(this.db, this.coll)
                .find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
                .rawData()
                .count(),
        );
        getTimeseriesCollForRawOps(this.db, this.coll).update(
            {"meta.sensorId": 2},
            {"$set": {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
            getRawOperationSpec(this.db),
        );

        const res = this.coll.validate();
        assert(!res.valid, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, res);
        assert.eq(res.errors.length, 1, res);

        const record = getTimeseriesCollForRawOps(this.db, this.coll).findOneWithRawData({"meta.sensorId": 2});
        assert(record);
        TimeseriesTest.checkForDocumentValidationFailureLog(this.coll, record);
    });

    it("Inserts enough documents to close a bucket, then manually changes the version to 1 and validation returns an error.", function () {
        this.coll.insertMany(
            [...Array(1200).keys()].map((i) => ({
                "metadata": {"sensorId": 3, "type": "temperature"},
                "timestamp": ISODate(),
                "temp": i,
            })),
            {ordered: false},
        );
        const allRecords = getTimeseriesCollForRawOps(this.db, this.coll)
            .find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
            .rawData()
            .toArray();
        assert.gt(allRecords.length, 1, allRecords); // Check that at least two buckets exist
        getTimeseriesCollForRawOps(this.db, this.coll).updateOne(
            {"meta.sensorId": 3, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
            {"$set": {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
            getRawOperationSpec(this.db),
        );
        const res = this.coll.validate();
        assert(!res.valid, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, res);
        assert.eq(res.errors.length, 1, res);

        const record = getTimeseriesCollForRawOps(this.db, this.coll).findOneWithRawData({"meta.sensorId": 3});
        TimeseriesTest.checkForDocumentValidationFailureLog(this.coll, record);
    });

    it("Validates an invalid timeseries buckets version and detects the error", function () {
        this.coll.insertMany(
            [...Array(1100).keys()].map((i) => ({
                "metadata": {"sensorId": 4, "type": "temperature"},
                "timestamp": ISODate(),
                "temp": i,
            })),
            {ordered: false},
        );
        getTimeseriesCollForRawOps(this.db, this.coll).updateOne(
            {"meta.sensorId": 4, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
            {"$set": {"control.version": 500}},
            getRawOperationSpec(this.db),
        );
        const res = this.coll.validate();
        assert(!res.valid, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, res);
        assert.eq(res.errors.length, 1, res);

        const record = getTimeseriesCollForRawOps(this.db, this.coll).find({"meta.sensorId": 4}).rawData().toArray()[0];
        TimeseriesTest.checkForDocumentValidationFailureLog(this.coll, record);
    });

    it("Validates and detects multiple errors with version mismatch", function () {
        this.coll.insertMany(
            [...Array(1100).keys()].map((i) => ({
                "metadata": {"sensorId": 5, "type": "temperature"},
                "timestamp": ISODate(),
                "temp": i,
            })),
            {ordered: false},
        );

        // Verify multiple buckets exist.
        assert.gt(
            getTimeseriesCollForRawOps(this.db, this.coll)
                .find({"meta.sensorId": 5, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
                .rawData()
                .count(),
            1,
        );

        // Make invalid record with non-existant version
        getTimeseriesCollForRawOps(this.db, this.coll).update(
            {"meta.sensorId": 5, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
            {"$set": {"control.version": 500}},
            getRawOperationSpec(this.db),
        );
        const record1 = getTimeseriesCollForRawOps(this.db, this.coll).findOneWithRawData({
            "meta.sensorId": 5,
            "control.version": 500,
        });

        // Make second invalid record with incompatible version.
        getTimeseriesCollForRawOps(this.db, this.coll).updateOne(
            {"meta.sensorId": 5, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
            {"$set": {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
            getRawOperationSpec(this.db),
        );
        const record2 = getTimeseriesCollForRawOps(this.db, this.coll).findOneWithRawData({
            "meta.sensorId": 5,
            "control.version": TimeseriesTest.BucketVersion.kUncompressed,
        });

        // Detect both records
        const res = this.coll.validate();
        assert(!res.valid, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 2, res);
        assert.eq(res.errors.length, 2, res);

        // Detect log lines for both records.
        TimeseriesTest.checkForDocumentValidationFailureLog(this.coll, record1);
        TimeseriesTest.checkForDocumentValidationFailureLog(this.coll, record2);
    });

    afterEach(function () {
        this.coll.drop();
    });

    after(function () {
        // As of SERVER-86451, time-series inconsistencies detected during validation
        // will error in testing, instead of being warnings. In this case,
        // validation on shutdown would fail, whereas before only a warning would be thrown.
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});
