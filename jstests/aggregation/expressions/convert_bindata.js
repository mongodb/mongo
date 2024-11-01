/**
 * Tests behavior of BinData $convert.
 * @tags: [
 *   # BinData $convert was added in v8.0.
 *   requires_fcv_80,
 * ]
 */

import {runConvertTests} from "jstests/libs/convert_shared.js";

const kUUIDSubtype = 4;
const kNonUUIDSubtype = 0;

const coll = db.expression_convert_bindata;
coll.drop();

const requiresFCV80 = true;

//
// One test document for each possible conversion. Edge cases for these conversions are tested
// in expression_convert_test.cpp.
//
const conversionTestDocs = [
    // 'to' can also be a sub-document describing the type.
    {_id: 1, input: 1.9, target: {type: "double"}, expected: 1.9},
    {_id: 2, input: 1.9, target: {type: "string"}, expected: "1.9"},
    {_id: 3, input: 1.9, target: {type: "bool"}, expected: true},
    {_id: 4, input: 1.9, target: {type: "date"}, expected: ISODate("1970-01-01T00:00:00.001Z")},
    {_id: 5, input: 1.9, target: {type: "int"}, expected: NumberInt(1)},
    {_id: 6, input: 1.9, target: {type: "long"}, expected: NumberLong(1)},
    {_id: 7, input: 1.9, target: {type: "decimal"}, expected: NumberDecimal(1.9)},

    // Test conversions from string to BinData.
    {
        _id: 8,
        input: "867dee52-c331-484e-92d1-c56479b8e67e",
        target: {type: "binData", subtype: kUUIDSubtype},
        format: "uuid",
        expected: BinData(kUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 9,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "base64",
        expected: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 10,
        input: "hn3uUsMxSE6S0cVkebjmfg",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "base64url",
        expected: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 11,
        input: "867DEE52C331484E92D1C56479B8E67E",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "hex",
        expected: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 12,
        input: "🙂😎",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "utf8",
        expected: BinData(kNonUUIDSubtype, "8J+ZgvCfmI4="),
    },

    // Test conversions from BinData to string.
    {
        _id: 13,
        input: UUID("867dee52-c331-484e-92d1-c56479b8e67e"),
        target: "string",
        format: "uuid",
        expected: "867dee52-c331-484e-92d1-c56479b8e67e",
    },
    {
        _id: 14,
        input: BinData(kUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: "string",
        format: "auto",
        expected: "867dee52-c331-484e-92d1-c56479b8e67e",
    },
    {
        _id: 15,
        input: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: "string",
        format: "auto",
        expected: "hn3uUsMxSE6S0cVkebjmfg==",
    },
    {
        _id: 16,
        input: BinData(kUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: "string",
        format: "base64",
        expected: "hn3uUsMxSE6S0cVkebjmfg==",
    },
    {
        _id: 17,
        input: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: "string",
        format: "base64url",
        expected: "hn3uUsMxSE6S0cVkebjmfg",
    },
    {
        _id: 18,
        input: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: "string",
        format: "hex",
        expected: "867DEE52C331484E92D1C56479B8E67E",
    },
    {
        _id: 19,
        input: BinData(kNonUUIDSubtype, "8J+ZgvCfmI4="),
        target: "string",
        format: "utf8",
        expected: "🙂😎",
    },

    // Test BinData identity conversions.
    {
        _id: 20,
        input: UUID("867dee52-c331-484e-92d1-c56479b8e67e"),
        target: {type: "binData", subtype: kUUIDSubtype},
        expected: BinData(kUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 21,
        input: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: {type: "binData", subtype: kNonUUIDSubtype},
        expected: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 22,
        input: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        // 'subtype' defaults to 0 (generic BinData).
        target: "binData",
        expected: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
];

//
// Unsupported conversions.
//
const illegalConversionTestDocs = [
    // Can only convert string (or BinData) to BinData.
    {
        _id: 3,
        input: ObjectId("0123456789abcdef01234567"),
        target: {type: "binData", subtype: kUUIDSubtype}
    },
    {
        _id: 4,
        input: ObjectId("0123456789abcdef01234567"),
        target: {type: "binData", subtype: kNonUUIDSubtype}
    },

    // Can't convert UUID string to non-UUID BinData.
    {
        _id: 5,
        input: "867dee52-c331-484e-92d1-c56479b8e67e",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "uuid"
    },

    // Input is not a valid UUID, base64, hex or utf8 string.
    {
        _id: 6,
        input: "867dee--52-c331-484e-",
        target: {type: "binData", subtype: kUUIDSubtype},
        format: "uuid"
    },
    {
        _id: 7,
        input: "867dee--52-c331-484e-",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "base64"
    },
    {
        _id: 8,
        input: "867dee--52-c331-484e-",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "base64url"
    },
    {
        _id: 9,
        input: "867dee--52-c331-484e-zx",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "hex"
    },

    // When converting from string to BinData, the "auto" format is not allowed, and the "uuid"
    // format is allowed iff the subtype is UUID.
    {
        _id: 10,
        input: "867dee52-c331-484e-92d1-c56479b8e67e",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "uuid"
    },
    {
        _id: 11,
        input: "867dee52-c331-484e-92d1-c56479b8e67e",
        target: {type: "binData", subtype: kNonUUIDSubtype},
        format: "auto"
    },
    {
        _id: 12,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        target: {type: "binData", subtype: kUUIDSubtype},
        format: "base64"
    },
    {
        _id: 13,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        target: {type: "binData", subtype: kUUIDSubtype},
        format: "auto"
    },

    // Forbidden conversions between different binData subtypes
    {
        _id: 14,
        input: UUID("867dee52-c331-484e-92d1-c56479b8e67e"),
        target: {type: "binData", subtype: kNonUUIDSubtype},
        expected: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
    },
    {
        _id: 15,
        input: BinData(kNonUUIDSubtype, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: {type: "binData", subtype: kUUIDSubtype},
    },
    {
        _id: 16,
        input: UUID("867dee52-c331-484e-92d1-c56479b8e67e"),
        target: {type: "binData", subtype: 255},
    },
    {
        _id: 17,
        input: BinData(255, "hn3uUsMxSE6S0cVkebjmfg=="),
        target: {type: "binData", subtype: 0},
    },
];

//
// Conversions with invalid 'to' argument.
//
const invalidTargetTypeDocs = [
    // Valid subtype is required when converting to BinData.
    {
        _id: 1,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        target: {type: "binData", subtype: -1},
        format: "base64",
        expectedCode: 4341107
    },
    {
        _id: 2,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        target: {type: "binData", subtype: 1000},
        format: "base64",
        expectedCode: 4341107
    },
    {
        _id: 3,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        // User-defined subtypes must be between 128-255.
        target: {type: "binData", subtype: 256},
        format: "base64",
        expectedCode: 4341107
    },
    {
        _id: 4,
        input: "hn3uUsMxSE6S0cVkebjmfg==",
        // User-defined subtypes must be between 128-255.
        target: {type: "binData", subtype: 127},
        format: "base64",
        expectedCode: 4341107
    },
    // Invalid type.
    {_id: 5, input: 123, target: {type: -2}, expectedCode: ErrorCodes.FailedToParse},
    {_id: 6, input: 123, target: -2, expectedCode: ErrorCodes.FailedToParse},
];

runConvertTests(
    {coll, requiresFCV80, conversionTestDocs, illegalConversionTestDocs, invalidTargetTypeDocs});
