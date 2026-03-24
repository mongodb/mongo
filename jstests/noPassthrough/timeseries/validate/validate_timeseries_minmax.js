/**
 * Tests that the validate command now checks that 'control.min' and 'control.max' fields in a
 * time-series bucket agree with those in 'data', and that it adds a warning and
 * increments the number of noncompliant documents if they don't.
 * @tags: [requires_fcv_62]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {describe, before, after, afterEach, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

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
        "temp": 15,
    },
];

const weatherDataWithGap = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 15,
    },
];

const stringData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "temp": 12,
        "str": "a",
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "temp": 16,
        "str": "11",
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "temp": 16,
        "str": "18",
    },
];

const arrayData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "arr": ["a", {"field": 1}, 1],
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "arr": ["1", {"field": 2}, 100],
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "arr": ["z", {"field": -2}, 30],
    },
];

const objectData = [
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "obj": {"nestedObj": {"field": 0}},
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "obj": {"nestedObj": {"field": 10}},
    },
    {
        "metadata": {"sensorId": 5578, "type": "temperature"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "obj": {"nestedObj": {"field": 2}},
    },
];

const lotsOfData = [...Array(1010).keys()].map((i) => ({
    "metadata": {"sensorId": 2, "type": "temperature"},
    "timestamp": ISODate(),
    "temp": i,
}));

const skipFieldData = [...Array(1010).keys()].map(function (i) {
    if (i % 2) {
        return {"timestamp": ISODate(), "temp": i};
    } else {
        return {"timestamp": ISODate()};
    }
});

const nestedArrayData = [
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T12:00:00.000Z"),
        arrObj: [{x: 1}, {y: [2, 3]}],
    },
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T16:00:00.000Z"),
        arrObj: [{x: 5}, {y: [4, 5]}],
    },
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T20:00:00.000Z"),
        arrObj: [{x: 3}, {y: [0, 1]}],
    },
];

const nestedObjectWithArrayData = [
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T12:00:00.000Z"),
        objArr: {nested: {arr: [1, 2], marker: "a"}},
    },
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T16:00:00.000Z"),
        objArr: {nested: {arr: [5, 6], marker: "z"}},
    },
    {
        metadata: {sensorId: 5578, type: "temperature"},
        timestamp: ISODate("2021-05-18T20:00:00.000Z"),
        objArr: {nested: {arr: [3, 4], marker: "m"}},
    },
];

/** Drops collection and creates it again with new data, checking that collection is valid before
 * faulty data is inserted.
 * @param {DB} db
 * @param {object} data
 */
function setUpCollection(db, data) {
    db.getCollection(collName).drop();
    assert.commandWorked(
        db.createCollection(collName, {
            timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
        }),
    );
    const collection = db.getCollection(collName);
    assert.commandWorked(collection.insertMany(data, {ordered: false}));

    // If we are always writing to time-series collections using the compressed format, replace the
    // compressed bucket with the decompressed bucket.
    const bucketDoc = getTimeseriesCollForRawOps(db, collection).find().rawData()[0];
    TimeseriesTest.decompressBucket(bucketDoc);
    getTimeseriesCollForRawOps(db, collection).replaceOne({_id: bucketDoc._id}, bucketDoc, getRawOperationSpec(db));

    const result = assert.commandWorked(collection.validate());
    assert(result.valid, tojson(result));
    assert(result.warnings.length == 0, tojson(result));
    assert(result.nNonCompliantDocuments == 0, tojson(result));
}

describe("Test 'control.min' and 'control.max' match the uncompressed documents in 'data'", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(jsTestName());
    });

    it("Updates 'control' min temperature with corresponding update in recorded temperature, testing that valid updates do not return warnings.", function () {
        setUpCollection(this.db, weatherData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.temp": 17}},
                getRawOperationSpec(this.db),
            ),
        );
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"data.temp.1": 17}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = false;
        assert(res.valid, tojson(res));
        assert(res.warnings.length == 0, tojson(res));
        assert(res.nNonCompliantDocuments == 0, tojson(res));
    });

    it("Fails validation if update to the 'control' min and max temperature without an update in recorded temperature.", function () {
        setUpCollection(this.db, weatherData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.min.temp": -200}},
                getRawOperationSpec(this.db),
            ),
        );
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.temp": 500}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation if updates to the 'control' min and max temperature without an update in recorded temperature, when there is also a gap in observed temperature (i.e, one or more of the data entries does not have a 'temp' field", function () {
        setUpCollection(this.db, weatherDataWithGap);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.min.temp": -200}},
                getRawOperationSpec(this.db),
            ),
        );
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.temp": 500}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation if updates to the 'control' max _id without an update in recorded temperature.", function () {
        setUpCollection(this.db, weatherData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max._id": ObjectId("62bcc728f3c51f43297eea43")}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation if updates to the 'control' min timestamp without an update in recorded temperature.", function () {
        // Updates the 'control' min timestamp without an update in recorded temperature.
        setUpCollection(this.db, weatherData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.min.timestamp": ISODate("2021-05-19T13:00:00.000Z")}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation if extra field is added to the 'control' min object.", function () {
        setUpCollection(this.db, weatherData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.min.extra": 10}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 2, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation when discrepancies with string min/max are found", function () {
        setUpCollection(this.db, stringData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.min.str": "-200"}},
                getRawOperationSpec(this.db),
            ),
        );
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.str": "zzz"}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation when discrepancies with array values are caught.", function () {
        setUpCollection(this.db, arrayData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.arr": ["zzzzz", {"field": -2}, 30]}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Fails validation when discrepancies with nested object are caught.", function () {
        setUpCollection(this.db, objectData);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.obj": {"nestedObj": {"field": 2}}}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert(res.errors.length == 1, tojson(res));
        assert(res.nNonCompliantDocuments == 1, tojson(res));
    });

    it("Tests 'control.version': 2 representing compressed buckets.", function () {
        setUpCollection(this.db, lotsOfData);
        const coll = this.db.getCollection(collName);
        getTimeseriesCollForRawOps(this.db, coll).updateOne(
            {"meta.sensorId": 2, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
            {"$set": {"control.max.temp": 800}},
            getRawOperationSpec(this.db),
        );
        const res = coll.validate();
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1);
        assert.eq(res.errors.length, 1);
    });

    it("Checks no errors are thrown with a valid closed bucket.", function () {
        setUpCollection(this.db, lotsOfData);
        const coll = this.db.getCollection(collName);
        const res = coll.validate();
        assert(res.valid, tojson(res));
        this.expectValidationFailure = false;
        assert.eq(res.nNonCompliantDocuments, 0);
        assert.eq(res.warnings.length, 0);
    });

    it("Checks no errors are thrown with a valid closed bucket.", function () {
        setUpCollection(this.db, lotsOfData);
        const coll = this.db.getCollection(collName);
        const res = coll.validate();
        assert(res.valid, tojson(res));
        this.expectValidationFailure = false;
        assert.eq(res.nNonCompliantDocuments, 0);
        assert.eq(res.warnings.length, 0);
    });

    it("Checks no errors are thrown with a valid closed bucket with skipped data fields.", function () {
        jsTestLog(
            "Running validate on a correct version 2 bucket with skipped data fields, checking that no warnings are found.",
        );
        setUpCollection(this.db, skipFieldData);
        const coll = this.db.getCollection(collName);
        const res = coll.validate();
        assert(res.valid, tojson(res));
        this.expectValidationFailure = false;
        assert.eq(res.nNonCompliantDocuments, 0);
        assert.eq(res.warnings.length, 0);
    });

    it("Passes validation when array min/max are updated consistently with the bucket data", function () {
        setUpCollection(this.db, arrayData);
        const coll = this.db.getCollection(collName);

        // After the data update, the element-wise min/max across all 3 entries is:
        //   data.arr.0 (unchanged): ["a", {field: 1}, 1]
        //   data.arr.1 (updated):   ["0", {field: 2}, 100]
        //   data.arr.2 (updated):   ["zzzzz", {field: -2}, 30]
        // Element-wise min: pos0="0"(arr.1), pos1={field:-2}(arr.2), pos2=1(arr.0)
        // Element-wise max: pos0="zzzzz"(arr.2), pos1={field:2}(arr.1), pos2=100(arr.1)
        const newMin = ["0", {field: -2}, 1];
        const newMax = ["zzzzz", {field: 2}, 100];

        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {
                    $set: {
                        "control.min.arr": newMin,
                        "control.max.arr": newMax,
                        "data.arr.1": ["0", {field: 2}, 100],
                        "data.arr.2": ["zzzzz", {field: -2}, 30],
                    },
                },
                getRawOperationSpec(this.db),
            ),
        );

        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = false;
        assert(res.valid, tojson(res));
        assert.eq(res.errors.length, 0, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 0, tojson(res));
    });

    it("Fails validation when control.max for an array has an extra trailing element", function () {
        setUpCollection(this.db, arrayData);
        const coll = this.db.getCollection(collName);

        // The actual element-wise max is ["z", {field: 2}, 100]. We add a trailing element
        // to specifically test the extra-element detection, keeping positions 0-2 correct.
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {$set: {"control.max.arr": ["z", {field: 2}, 100, 999]}},
                getRawOperationSpec(this.db),
            ),
        );

        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        // Two errors: one for the min/max mismatch, one for the structural mismatch between
        // control.max (4 elements) and control.min (3 elements) detected by
        // doesMinMaxHaveMixedSchemaData ("Encountered extra field(s) in time-series bucket
        // min/max").
        assert.eq(res.errors.length, 2, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, tojson(res));
    });

    it("Fails validation when control.min for an array of objects differs in a nested object value", function () {
        setUpCollection(this.db, nestedArrayData);
        const coll = this.db.getCollection(collName);

        // The actual element-wise min is [{x: 1}, {y: [0, 1]}]. Corrupt only the first
        // element's nested field value while keeping the second element correct.
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {$set: {"control.min.arrObj": [{x: -100}, {y: [0, 1]}]}},
                getRawOperationSpec(this.db),
            ),
        );

        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert.eq(res.errors.length, 1, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, tojson(res));
    });

    it("Passes validation when object min/max are updated consistently with the bucket data", function () {
        setUpCollection(this.db, objectData);
        const coll = this.db.getCollection(collName);

        const newMax = {nestedObj: {field: 20}};

        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {
                    $set: {
                        "control.max.obj": newMax,
                        "data.obj.1": newMax,
                    },
                },
                getRawOperationSpec(this.db),
            ),
        );

        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = false;
        assert(res.valid, tojson(res));
        assert.eq(res.errors.length, 0, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 0, tojson(res));
    });

    it("Fails validation when control.max for an object has an extra nested field", function () {
        setUpCollection(this.db, objectData);
        const coll = this.db.getCollection(collName);

        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {$set: {"control.max.obj": {nestedObj: {field: 10, extra: 1}}}},
                getRawOperationSpec(this.db),
            ),
        );

        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        // Two errors: one for the min/max mismatch, one for the structural mismatch between
        // control.max.obj.nestedObj (has extra field 'extra') and control.min.obj.nestedObj
        // detected by doesMinMaxHaveMixedSchemaData ("Encountered extra field(s)...").
        assert.eq(res.errors.length, 2, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, tojson(res));
    });

    it("Fails validation when control.max for an object containing arrays differs from the bucket data", function () {
        setUpCollection(this.db, nestedObjectWithArrayData);
        const coll = this.db.getCollection(collName);

        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {$set: {"control.max.objArr": {nested: {arr: [9, 9], marker: "z"}}}},
                getRawOperationSpec(this.db),
            ),
        );

        const res = assert.commandWorked(coll.validate());
        this.expectValidationFailure = true;
        assert(!res.valid, tojson(res));
        assert.eq(res.errors.length, 1, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, tojson(res));
    });

    afterEach(function () {
        if (this.expectValidationFailure) {
            const coll = this.db.getCollection(collName);
            const record = getTimeseriesCollForRawOps(this.db, coll).find().rawData().toArray().at(-1);
            TimeseriesTest.checkForDocumentValidationFailureLog(coll, record);
        }
    });

    after(function () {
        // As of SERVER-86451, time-series inconsistencies detected during validation
        // will error in testing, instead of being warnings. In this case,
        // validation on shutdown would fail, whereas before only a warning would be thrown.
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});
