/**
 * Tests converting an Object to BinData using $convert.
 * When the input resolves to an Object and the target type is BinData, $convert returns the raw
 * BSON representation of the input.
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagConvertObjectToBinData,
 * ]
 */

import {runConvertTests} from "jstests/libs/query/convert_shared.js";

const coll = db.expression_convert_object_to_bindata;
coll.drop();

//
// One test document for each conversion from Object to BinData.
// Edge cases are tested in evaluate_convert_test.cpp.
//
const conversionTestDocs = [
    // Empty object.
    {_id: 0, input: {}, target: "binData", expected: BinData(0, "BQAAAAA=")},

    // Int32 vs Long produce different BSON.
    {_id: 1, input: {a: NumberInt(1)}, target: "binData", expected: BinData(0, "DAAAABBhAAEAAAAA")},
    {
        _id: 2,
        input: {a: NumberLong(1)},
        target: "binData",
        expected: BinData(0, "EAAAABJhAAEAAAAAAAAAAA=="),
    },

    // Field order matters.
    {
        _id: 3,
        input: {a: "a", b: "b"},
        target: "binData",
        expected: BinData(0, "FwAAAAJhAAIAAABhAAJiAAIAAABiAAA="),
    },
    {
        _id: 4,
        input: {b: "b", a: "a"},
        target: "binData",
        expected: BinData(0, "FwAAAAJiAAIAAABiAAJhAAIAAABhAAA="),
    },

    // Array element order matters.
    {
        _id: 5,
        input: {a: [NumberInt(1), NumberInt(2)]},
        target: "binData",
        expected: BinData(0, "GwAAAARhABMAAAAQMAABAAAAEDEAAgAAAAAA"),
    },
    {
        _id: 6,
        input: {a: [NumberInt(2), NumberInt(1)]},
        target: "binData",
        expected: BinData(0, "GwAAAARhABMAAAAQMAACAAAAEDEAAQAAAAAA"),
    },

    // Custom subtype via {to: {type, subtype}}.
    {
        _id: 7,
        input: {a: "a"},
        target: {type: "binData", subtype: 128},
        expected: BinData(128, "DgAAAAJhAAIAAABhAAA="),
    },

    // Complex nested object with string, nested doc, array, and binData.
    {
        _id: 8,
        input: {
            "a": "hello",
            "b": {"x": NumberInt(42), "y": [NumberInt(1), NumberInt(2), NumberInt(3)]},
            "c": BinData(0, "AQIDBA=="),
        },
        target: "binData",
        expected: BinData(
            0,
            "SgAAAAJhAAYAAABoZWxsbwADYgApAAAAEHgAKgAAAAR5ABoAAAAQMAABAAAAEDEAAgAAABAyAAMAAAAAAAVjAAQAAAAAAQIDBAA=",
        ),
    },

    // String value containing an embedded null byte.
    {
        _id: 9,
        input: {"a": "hel\x00lo"},
        target: "binData",
        expected: BinData(0, "EwAAAAJhAAcAAABoZWwAbG8AAA=="),
    },

    // String value that is just a null byte.
    {
        _id: 10,
        input: {"a": "\x00"},
        target: "binData",
        expected: BinData(0, "DgAAAAJhAAIAAAAAAAA="),
    },

    // 'format' is validated but ignored for object -> binData.
    {
        _id: 11,
        input: {a: "a"},
        target: "binData",
        format: "hex",
        expected: BinData(0, "DgAAAAJhAAIAAABhAAA="),
    },
];

const invalidArgumentValueDocs = [
    {_id: 0, input: {a: "a"}, target: {type: "binData", subtype: 50}, expectedCode: 4341107},
    {_id: 1, input: {a: "a"}, target: {type: "binData", subtype: "zero"}, expectedCode: 4341108},
    {_id: 2, input: {a: "a"}, target: "binData", format: "bson", expectedCode: 4341125},
    {_id: 3, input: {a: "a"}, target: "binData", format: 123, expectedCode: 4341114},
];

const illegalConversionTestDocs = [
    {_id: 0, input: true, target: "binData"},
    {_id: 1, input: new Date("2024-01-01"), target: "binData"},
    {_id: 2, input: ObjectId("000000000000000000000000"), target: "binData"},
    {_id: 3, input: /abc/, target: "binData"},
    {_id: 4, input: Timestamp(0, 1), target: "binData"},
];

const nullTestDocs = [{_id: 0, input: null}, {_id: 1, input: undefined}, {_id: 2 /* input is missing */}];

runConvertTests({
    coll,
    requiresFCV80: true,
    conversionTestDocs,
    invalidArgumentValueDocs,
    illegalConversionTestDocs,
    nullTestDocs,
    runShorthandTests: false,
});
