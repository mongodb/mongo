/**
 * Tests directly inserting a time-series bucket with docs incrementally to
 * exercise rollover logic.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_getmore,
 *   # TODO(SERVER-108445) Reenable this test
 *   multiversion_incompatible,
 * ]
 */

import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

/* Helper for fetching server parameters */
function getParameter(db, param) {
    const res = db.adminCommand({getParameter: 1, [param]: 1});
    assert.commandWorked(res);
    return res[param];
}

describe("Timeseries rollover test suite", function () {
    beforeEach(function () {
        this.standalone = MongoRunner.runMongod();

        this.dbName = jsTestName();
        this.testDB = this.standalone.getDB(this.dbName);
        this.collName = "ts";
        assert.commandWorked(this.testDB.dropDatabase());
        this.testDB.getCollection(this.collName).drop();
        assert.commandWorked(
            this.testDB.createCollection(this.collName, {
                timeseries: {timeField: "t", metaField: "m", granularity: "seconds", bucketMaxSpanSeconds: 3600},
            }),
        );
    });

    it("Test rollover by count", function () {
        const maxCount = getParameter(this.testDB, "timeseriesBucketMaxCount");
        const coll = this.testDB[this.collName];

        let doc = {
            t: ISODate("2025-01-01T12:00:00"),
            m: "meta",
            a: "foo",
        };
        assert.commandWorked(coll.insert(doc));
        assert.eq(1, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // up to count remains in same bucket
        for (let i = 2; i <= maxCount; i++) {
            assert.commandWorked(coll.insert(doc));
            // Check doc count periodically to avoid slowing down the test too much.
            if (i % 100 === 0) {
                assert.eq(i, coll.find({"m": "meta"}).toArray().length);
            }
            assert.eq(
                1,
                getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length,
            );
        }

        // next measurement, should rollover
        assert.commandWorked(coll.insert(doc));
        assert.eq(maxCount + 1, coll.find({"m": "meta"}).toArray().length);
        assert.eq(2, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);
    });

    it("Test rollover by size", function () {
        const maxSize = getParameter(this.testDB, "timeseriesBucketMaxSize");
        const minCount = getParameter(this.testDB, "timeseriesBucketMinCount");

        const coll = this.testDB[this.collName];

        let doc = {
            t: ISODate("2025-01-01T12:00:00"),
            m: "meta",
            a: "foo",
        };
        assert.commandWorked(coll.insert(doc));
        assert.eq(1, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // large measurement on 2nd insert, stays in same bucket
        doc.payload = "x".repeat(maxSize); // large payload
        assert.commandWorked(coll.insert(doc));
        assert.eq(2, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // up to mincount remains in same bucket
        doc.payload = "x"; // small payload
        for (let i = 3; i <= minCount; i++) {
            assert.commandWorked(coll.insert(doc));
        }
        assert.eq(minCount, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // next measurement, should rollover
        assert.commandWorked(coll.insert(doc));
        assert.eq(minCount + 1, coll.find({"m": "meta"}).toArray().length);
        assert.eq(2, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);
    });

    it("Test rollover by time forward", function () {
        const coll = this.testDB.getCollection(this.collName);

        let doc = {
            t: ISODate("2025-01-01T12:00:00"),
            m: "meta",
            a: "foo",
        };
        assert.commandWorked(coll.insert(doc));
        assert.eq(1, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // new measurement, same bucket
        doc.t = ISODate("2025-01-01T12:30:00");
        assert.commandWorked(coll.insert(doc));
        assert.eq(2, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // new measurement, same bucket, max span
        doc.t = ISODate("2025-01-01T12:59:59");
        assert.commandWorked(coll.insert(doc));
        assert.eq(3, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // new measurement, time beyond max span, should rollover
        doc.t = ISODate("2025-01-01T13:00:00");
        assert.commandWorked(coll.insert(doc));
        assert.eq(4, coll.find({"m": "meta"}).toArray().length);
        assert.eq(2, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);
    });

    it("Test rollover by time backward", function () {
        const coll = this.testDB[this.collName];

        let doc = {
            t: ISODate("2025-01-01T12:00:00"),
            m: "meta",
            a: "foo",
        };
        assert.commandWorked(coll.insert(doc));
        assert.eq(1, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // new measurement, same bucket
        doc.t = ISODate("2025-01-01T12:30:00");
        assert.commandWorked(coll.insert(doc));
        assert.eq(2, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // new measurement, same bucket, max span
        doc.t = ISODate("2025-01-01T12:59:59");
        assert.commandWorked(coll.insert(doc));
        assert.eq(3, coll.find({"m": "meta"}).toArray().length);
        assert.eq(1, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);

        // new measurement, time backward, should rollover
        doc.t = ISODate("2025-01-01T11:59:59");
        assert.commandWorked(coll.insert(doc));
        assert.eq(4, coll.find({"m": "meta"}).toArray().length);
        assert.eq(2, getTimeseriesCollForRawOps(this.testDB, coll).find({"meta": "meta"}).rawData().toArray().length);
    });

    afterEach(function () {
        MongoRunner.stopMongod(this.standalone);
    });
});
