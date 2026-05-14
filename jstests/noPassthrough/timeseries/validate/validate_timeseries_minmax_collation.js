/**
 * Repro for SERVER-125806: the validate command must honor the collection's collation when
 * comparing recomputed min/max against the bucket's stored control.min/control.max. Under a
 * case-insensitive collation, byte-different strings (e.g. "berlin" vs "Berlin") compare equal
 * for purposes of min/max, so the first-scanned casing is what lands in control.min/max. A
 * byte-only comparison in the validator produces a false-positive mismatch warning even though
 * the bucket is consistent.
 *
 * Before the fix at src/mongo/db/validate/validate_adaptor.cpp L461-L470, this test fails with
 * a non-compliant document on the case-insensitive collection. After the fix it must pass with
 * zero warnings and zero non-compliant documents.
 *
 * @tags: [requires_fcv_62]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {describe, before, after, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

// Case-insensitive collation: "Berlin" == "berlin" == "BERLIN".
const caseInsensitive = {
    locale: "en_US",
    strength: 2,
};

// Diacritic-insensitive collation: "a" == "á" == "ä".
const diacriticInsensitive = {
    locale: "en_US",
    strength: 1,
};

// All three records compare equal under caseInsensitive on the 'notes' field, so they all
// land in the same bucket and the bucket's control.min.notes == control.max.notes == the
// first-scanned form ("Successful login from Berlin, Berlin, DE"). An element-wise recompute
// over the data values yields a different byte sequence (e.g. the lower-cased version), which
// is logically equal under the collation but byte-different — the false-positive trigger.
const mixedCaseData = [
    {
        "metadata": {"sensorId": 5578, "type": "audit"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "notes": "Successful login from Berlin, Berlin, DE",
    },
    {
        "metadata": {"sensorId": 5578, "type": "audit"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "notes": "successful login from berlin, berlin, de",
    },
    {
        "metadata": {"sensorId": 5578, "type": "audit"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "notes": "SUCCESSFUL LOGIN FROM BERLIN, BERLIN, DE",
    },
];

const mixedDiacriticData = [
    {
        "metadata": {"sensorId": 5578, "type": "audit"},
        "timestamp": ISODate("2021-05-18T12:00:00.000Z"),
        "city": "Zurich",
    },
    {
        "metadata": {"sensorId": 5578, "type": "audit"},
        "timestamp": ISODate("2021-05-18T16:00:00.000Z"),
        "city": "Zürich",
    },
    {
        "metadata": {"sensorId": 5578, "type": "audit"},
        "timestamp": ISODate("2021-05-18T20:00:00.000Z"),
        "city": "zürich",
    },
];

/**
 * Drops and recreates a time-series collection with the supplied collation, inserts data,
 * decompresses the resulting bucket so subsequent updates are visible, and asserts the
 * starting state is valid under the bucket validator.
 */
function setUpCollection(db, data, collation) {
    db.getCollection(collName).drop();
    assert.commandWorked(
        db.createCollection(collName, {
            timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
            collation: collation,
        }),
    );
    const collection = db.getCollection(collName);
    assert.commandWorked(collection.insertMany(data, {ordered: false}));

    // Replace compressed bucket with its decompressed form, matching the
    // validate_timeseries_minmax.js sibling so raw updates remain inspectable.
    const bucketDoc = getTimeseriesCollForRawOps(db, collection).find().rawData()[0];
    TimeseriesTest.decompressBucket(bucketDoc);
    getTimeseriesCollForRawOps(db, collection).replaceOne(
        {_id: bucketDoc._id},
        bucketDoc,
        getRawOperationSpec(db),
    );

    // Sanity: pre-corruption state is valid. If this assertion fails the test environment is
    // already broken — it's not the bug under test.
    const result = assert.commandWorked(collection.validate());
    assert(result.valid, tojson(result));
    assert.eq(result.warnings.length, 0, tojson(result));
    assert.eq(result.nNonCompliantDocuments, 0, tojson(result));
}

describe("SERVER-125806: validator honors collection collation when comparing control min/max", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(jsTestName());
    });

    it("Passes validation for a case-insensitive bucket where data strings differ only in case", function () {
        setUpCollection(this.db, mixedCaseData, caseInsensitive);
        const coll = this.db.getCollection(collName);

        // Before the fix: validate reports a false-positive min/max mismatch because the
        // recomputed observed min/max (one casing) is byte-different from the stored
        // control.min/max (a different casing), even though they're equal under the
        // collation. After the fix this returns valid with zero warnings.
        const res = assert.commandWorked(coll.validate());
        assert(res.valid, tojson(res));
        assert.eq(res.warnings.length, 0, tojson(res));
        assert.eq(res.errors.length, 0, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 0, tojson(res));
    });

    it("Passes validation for a diacritic-insensitive bucket where data strings differ only in diacritics", function () {
        setUpCollection(this.db, mixedDiacriticData, diacriticInsensitive);
        const coll = this.db.getCollection(collName);

        const res = assert.commandWorked(coll.validate());
        assert(res.valid, tojson(res));
        assert.eq(res.warnings.length, 0, tojson(res));
        assert.eq(res.errors.length, 0, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 0, tojson(res));
    });

    it("Still detects a true min/max mismatch under a non-default collation (negative control)", function () {
        // Confirms the fix does not over-correct into ignoring real corruption. Even under a
        // case-insensitive collation, replacing control.max.notes with a string that does NOT
        // compare equal to any data value must still surface as an error.
        setUpCollection(this.db, mixedCaseData, caseInsensitive);
        const coll = this.db.getCollection(collName);
        assert.commandWorked(
            getTimeseriesCollForRawOps(this.db, coll).update(
                {},
                {"$set": {"control.max.notes": "zzz unrelated string"}},
                getRawOperationSpec(this.db),
            ),
        );
        const res = assert.commandWorked(coll.validate());
        assert(!res.valid, tojson(res));
        assert.eq(res.errors.length, 1, tojson(res));
        assert.eq(res.nNonCompliantDocuments, 1, tojson(res));
    });

    after(function () {
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});
