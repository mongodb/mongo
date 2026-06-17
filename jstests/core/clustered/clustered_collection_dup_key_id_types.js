/**
 * A duplicate insert into a clustered collection must report a DuplicateKey (E11000) error for
 * every form of cluster key value, rather than a "KeyString format error".
 *
 * A clustered collection's RecordId is the KeyString of the document's _id with the TypeBits
 * discarded (record_id_helpers::keyForElem). The duplicate-key error message used to be built by
 * decoding that RecordId back to BSON, which cannot recover types whose KeyString needs TypeBits:
 * a Decimal128, or a non-integral double, anywhere in the cluster key would throw Location50810 /
 * Location50825 instead of surfacing a clean DuplicateKey. During initial sync that thrown
 * exception fataled oplog application, so the node could never finish syncing.
 *
 * Regression test for SERVER-128392.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_fcv_90,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];

// Each entry is an _id value to insert twice. The second insert is always a duplicate and must
// fail with DuplicateKey. The "regression" cases all threw a KeyString format error before the fix.
const cases = [
    // Integral and non-numeric values that always round-tripped correctly.
    {desc: "int", id: NumberInt(7)},
    {desc: "long", id: NumberLong(7)},
    {desc: "whole double", id: 7.0},
    {desc: "whole decimal", id: NumberDecimal("7")},
    {desc: "string", id: "abc"},
    {desc: "objectid", id: new ObjectId()},
    {desc: "date", id: new ISODate("2026-01-01T00:00:00Z")},
    {desc: "timestamp", id: new Timestamp(1, 1)},
    {desc: "bool", id: true},
    {desc: "bindata", id: BinData(0, "aaaaaaaa")},
    {desc: "null", id: null},
    {desc: "object of integral", id: {a: NumberInt(3), b: "x"}},

    // Regressions: non-integral doubles (>= 1 -> Location50810, < 1 -> Location50825).
    {desc: "double > 1", id: 1.5},
    {desc: "double < 1", id: 0.5},
    {desc: "negative double", id: -1.5},
    {desc: "tiny double", id: 1e-40},

    // Regressions: Decimal128 values, including ones whose magnitude crosses both decode paths.
    {desc: "decimal > 1", id: NumberDecimal("1.13")},
    {desc: "decimal < 1", id: NumberDecimal("0.64")},
    {desc: "negative decimal", id: NumberDecimal("-1.13")},
    {desc: "decimal with trailing zeros", id: NumberDecimal("2.00")},
    {desc: "tiny decimal", id: NumberDecimal("1E-40")},
    {desc: "high precision decimal", id: NumberDecimal("1234567890.123456789012345")},

    // Regressions: the offending value nested inside a compound/array _id at various depths.
    {desc: "object with non-integral double", id: {a: 1.5}},
    {desc: "object with decimal", id: {a: NumberDecimal("1.13")}},
    {desc: "deeply nested decimal", id: {a: {b: {c: NumberDecimal("0.64")}}}},
    {desc: "array element non-integral double", id: {a: [1, 2.5]}},
    {desc: "array element decimal", id: {a: [NumberDecimal("1.13")]}},

    // A real-world compound _id shape with a Decimal128 nested in a subdocument.
    {
        desc: "compound _id with nested decimal",
        id: {
            operation_id: "a3bfdaef-73ea-4a65-9f49-c93666b214a5",
            item_no_short: "0206381",
            sub_item_no: 0,
            real_case_size: {v: NumberDecimal("1.13"), ex: false},
            order: 106,
        },
    },
];

describe("clustered collection duplicate _id key error", function () {
    beforeEach(function () {
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {
                clusteredIndex: {key: {_id: 1}, name: "_id_", unique: true},
            }),
        );
    });

    afterEach(function () {
        coll.drop();
    });

    for (const {desc, id} of cases) {
        it(`reports DuplicateKey for ${desc}`, function () {
            assert.commandWorked(coll.insert({_id: id}));
            const res = coll.insert({_id: id});
            assert.writeErrorWithCode(
                res,
                ErrorCodes.DuplicateKey,
                `duplicate _id (${desc}) should be DuplicateKey`,
            );
        });
    }

    it("surfaces the original cluster key value in the error message", function () {
        // The fix recovers the _id from the inserted document, so the duplicate-key message shows
        // the true value (with its original type) rather than failing to decode the RecordId.
        assert.commandWorked(coll.insert({_id: NumberDecimal("1.13")}));
        const err = coll.insert({_id: NumberDecimal("1.13")}).getWriteError();
        assert.eq(err.code, ErrorCodes.DuplicateKey, err);
        assert(/1\.13/.test(err.errmsg), "error message should contain the duplicated value", {
            err,
        });
    });
});
