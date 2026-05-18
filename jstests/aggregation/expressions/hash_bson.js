/**
 * Tests composing $convert (Object -> BinData) with $hash/$hexHash to hash entire documents by their
 * raw BSON representation.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagConvertObjectToBinData,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const collName = jsTestName();
const coll = db[collName];

function bsonHash(doc, algorithm, {hex}) {
    assertDropCollection(db, collName);
    assert.commandWorked(coll.insert(doc));
    const op = hex ? "$hexHash" : "$hash";
    return coll
        .aggregate([
            {
                $replaceRoot: {
                    newRoot: {
                        hash: {
                            [op]: {
                                input: {$convert: {input: "$$ROOT", to: "binData"}},
                                algorithm: algorithm,
                            },
                        },
                    },
                },
            },
        ])
        .toArray()[0].hash;
}

//
// Known hash values for each (doc, algorithm) pair. The hash is computed over the full stored
// BSON document (with _id first), so these golden values pin the exact BSON encoding.
//
const knownHashTests = [
    // Simple string _id.
    {doc: {_id: "foo"}, algorithm: "xxh64", expectedHex: "BCF618EB9A0147DA"},
    {
        doc: {_id: "foo"},
        algorithm: "sha256",
        expectedHex: "89CEF31BCB097DC880D1A08D795CA7F393507A2E80D130868F88BEEC70738212",
    },

    // Field order sensitivity: {a, b} and {b, a} must produce different hashes since BSON
    // encodes field order.
    {
        doc: {a: NumberInt(1), b: NumberInt(2), _id: "order_ab"},
        algorithm: "xxh64",
        expectedHex: "B73384BF8C856E8E",
    },
    {
        doc: {b: NumberInt(2), a: NumberInt(1), _id: "order_ba"},
        algorithm: "xxh64",
        expectedHex: "195166C39274581D",
    },

    // Type sensitivity: NumberLong(1) and NumberInt(1) have different BSON encodings (int64 vs
    // int32), so their hashes must differ.
    {doc: {_id: NumberLong(1)}, algorithm: "xxh64", expectedHex: "A22C0EEC15058460"},
    {
        doc: {_id: NumberLong(1)},
        algorithm: "sha256",
        expectedHex: "B5C1CE5FBD9EA13C8DCD31161DA48A61ADC1A0DD8F499C540692CD3772F43464",
    },
    {doc: {_id: NumberInt(1)}, algorithm: "xxh64", expectedHex: "3C71997DAA2D4ED9"},
    {
        doc: {_id: NumberInt(1)},
        algorithm: "sha256",
        expectedHex: "2FE93D7561368009C932FFEF6A24DBBFFF5802E05AB8939CF195D7A3912FBA7C",
    },

    // Nested object _id.
    {doc: {_id: {foo: NumberInt(1)}}, algorithm: "xxh64", expectedHex: "89DB1BF8D5E6962B"},
    {
        doc: {_id: {foo: NumberInt(1)}},
        algorithm: "sha256",
        expectedHex: "4A18340B667960C2CCA9FA2F3B4F79738112B0137B554DA91F7C0EE8DD0FAA38",
    },

    // Multi-field document with nested subdocument, array, and mixed types.
    {
        doc: {
            _id: "complex",
            name: "test",
            count: NumberInt(42),
            tags: ["a", "b"],
            nested: {x: NumberLong(100), y: true},
        },
        algorithm: "xxh64",
        expectedHex: "0F29469CACF5CF42",
    },
    {
        doc: {
            _id: "complex",
            name: "test",
            count: NumberInt(42),
            tags: ["a", "b"],
            nested: {x: NumberLong(100), y: true},
        },
        algorithm: "sha256",
        expectedHex: "5F65748D73F93A09DF5FC8B6516A02D766C55B55AFD02BFF69DF39E40BE44CB6",
    },
];

describe("BSON hash known values", () => {
    for (const {doc, algorithm, expectedHex} of knownHashTests) {
        it(`${algorithm} of ${tojson(doc)}`, () => {
            assert.eq(bsonHash(doc, algorithm, {hex: true}), expectedHex);
        });
    }
});

describe("$hash vs $hexHash consistency", () => {
    for (const alg of ["xxh64", "sha256", "md5"]) {
        it(`$hexHash returns the hex encoding of $hash output for ${alg}`, () => {
            const doc = {_id: "consistency_" + alg};
            const hexResult = bsonHash(doc, alg, {hex: true});
            const binResult = bsonHash(doc, alg, {hex: false});

            assert.eq(typeof hexResult, "string", `$hexHash(${alg}) should return a string`);
            assert(binResult instanceof BinData, `$hash(${alg}) should return BinData`);
            assert.eq(hexResult, binResult.hex().toUpperCase(), `mismatch for algorithm ${alg}`);
        });
    }
});
