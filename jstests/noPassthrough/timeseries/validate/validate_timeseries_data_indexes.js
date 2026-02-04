/**
 * Tests that the validate command now checks the indexes in the time-series buckets data fields.
 *
 * @tags: [requires_fcv_62]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {describe, before, beforeEach, after, afterEach, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const collPrefix = jsTestName();
let collName = collPrefix;
let testCount = 0;

const weatherData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "temp": 16,
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "humidity": 15,
    },
];

describe("Tests validate command checks indexes in the time-series buckets data fields.", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(jsTestName());
    });

    beforeEach(function () {
        // Drops collection and creates it again with new data, checking that collection is valid before
        // faulty data is inserted.
        testCount += 1;
        collName = collPrefix + testCount;
        this.db.getCollection(collName).drop();
        assert.commandWorked(
            this.db.createCollection(collName, {
                timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
            }),
        );
        const collection = this.db.getCollection(collName);
        assert.commandWorked(collection.insertMany(weatherData));
        const result = assert.commandWorked(collection.validate());
        assert(result.valid, tojson(result));
        assert(result.warnings.length == 0, tojson(result));
        assert(result.nNonCompliantDocuments == 0, tojson(result));

        // If we are always writing to time-series collections using the compressed format, replace the
        // compressed bucket with the decompressed bucket. This allows this test to directly make
        // updates to bucket measurements in order to test validation.
        const bucketDoc = getTimeseriesCollForRawOps(this.db, collection).findOneWithRawData();
        TimeseriesTest.decompressBucket(bucketDoc);
        getTimeseriesCollForRawOps(this.db, collection).replaceOne(
            {_id: bucketDoc._id},
            bucketDoc,
            getRawOperationSpec(this.db),
        );

        this.targetBucketDocId = bucketDoc._id;
        this.coll = this.db.getCollection(collName);
    });

    it("Fails to validate on bucket with non-numerical data field indexes on collection", function () {
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$rename": {"data.temp.0": "data.temp.a"}},
                getRawOperationSpec(this.db),
            ),
        );
    });

    it("Fails to validate on bucket with non-increasing data field indexes on collection", function () {
        // Keeps indexes from being reordered by Javascript.
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$rename": {"data.temp.0": "data.temp.a", "data.temp.1": "data.temp.b"}},
                getRawOperationSpec(this.db),
            ),
        );
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$rename": {"data.temp.a": "data.temp.1"}},
                getRawOperationSpec(this.db),
            ),
        );
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$rename": {"data.temp.b": "data.temp.0"}},
                getRawOperationSpec(this.db),
            ),
        );
    });

    it("Fails to validate for Out-of-Range Index", function () {
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$rename": {"data.temp.1": "data.temp.10"}},
                getRawOperationSpec(this.db),
            ),
        );
    });

    it("Fails to validate for Negative Index", function () {
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$rename": {"data.temp.0": "data.temp.-1"}},
                getRawOperationSpec(this.db),
            ),
        );
    });

    it("Fails validation for missing timeField index", function () {
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, this.coll).update(
                {},
                {"$unset": {"data.timestamp.1": ""}},
                getRawOperationSpec(this.db),
            ),
        );
    });

    afterEach(function () {
        printjson(getTimeseriesCollForRawOps(this.db, this.coll).find().rawData().toArray());
        const res = assert.commandWorked(this.coll.validate());
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));

        const record = getTimeseriesCollForRawOps(this.db, this.coll).findOneWithRawData();
        TimeseriesTest.checkForDocumentValidationFailureLog(this.coll, record);
    });

    after(function () {
        // As of SERVER-86451, time-series inconsistencies detected during validation
        // will error in testing, instead of being warnings. In this case,
        // validation on shutdown would fail, whereas before only a warning would be thrown.
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});
