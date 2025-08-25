/**
 * Tests behavior of conversions to string that were added in SERVER-46079.
 *
 * @tags: [
 *   featureFlagMqlJsEngineGap,
 *   requires_fcv_83,
 * ]
 */

import {runConvertTests} from "jstests/libs/query/convert_shared.js";

const coll = db.expression_convert_bindata;
coll.drop();

const requiresFCV83 = true;

function makeNestedArray(depth) {
    if (depth < 1) {
        return [depth];
    }
    return [depth, makeNestedArray(depth - 1)];
}

function makeNestedObject(depth) {
    if (depth < 1) {
        return {depth};
    }
    return {depth, f: makeNestedObject(depth - 1)};
}

// The limit is 150 in the legacy shell but we need to leave some wiggle room since this will be
// placed inside nested documents.
const nestedDepth140 = {
    arr: makeNestedArray(140),
    obj: makeNestedObject(140),
};

//
// One test document for each possible conversion. Edge cases for these conversions are tested
// in expression_convert_test.cpp.
//
const conversionTestDocs = [
    // Non-nested conversions that were missing before SERVER-46079.
    {_id: 1, input: /B*/is, target: "string", expected: "/B*/is"},
    {_id: 2, input: Timestamp(1, 2), target: "string", expected: "Timestamp(1, 2)"},
    {
        _id: 3,
        input: new DBPointer("coll", new ObjectId("0102030405060708090a0b0c")),
        target: "string",
        expected: 'DBRef("coll", 0102030405060708090a0b0c)',
    },
    {_id: 4, input: function () {}, target: "string", expected: "function () {}"},
    {_id: 5, input: MinKey, target: "string", expected: "MinKey"},
    {_id: 6, input: MaxKey, target: "string", expected: "MaxKey"},
    // Ensure we escape strings properly.
    {
        _id: 7,
        input: function () {
            return '""';
        },
        target: "string",
        expected: "function () {\n            return '\"\"';\n        }",
    },
    {
        _id: 8,
        input: new DBPointer('co"ll', new ObjectId("0102030405060708090a0b0c")),
        target: "string",
        expected: 'DBRef("co"ll", 0102030405060708090a0b0c)',
    },
    // Nested conversions.
    {_id: 9, input: {}, target: "string", expected: "{}"},
    {_id: 10, input: [], target: "string", expected: "[]"},
    {_id: 11, input: {foo: "bar"}, target: "string", expected: '{"foo":"bar"}'},
    {_id: 12, input: {foo: true}, target: "string", expected: '{"foo":true}'},
    {_id: 13, input: {foo: 123}, target: "string", expected: '{"foo":123}'},
    {_id: 14, input: {foo: 1.123123}, target: "string", expected: '{"foo":1.123123}'},
    {
        _id: 15,
        input: {foo: NumberDecimal("1.123123")},
        target: "string",
        expected: '{"foo":1.123123}',
    },
    {
        _id: 16,
        input: ["foo", {bar: "baz"}],
        target: "string",
        expected: '["foo",{"bar":"baz"}]',
    },
    {
        _id: 17,
        input: {
            objectId: ObjectId("507f1f77bcf86cd799439011"),
            uuid: UUID("3b241101-e2bb-4255-8caf-4136c566a962"),
            date: ISODate("2018-03-27T16:58:51.538Z"),
            regex: /^ABC/i,
            js: function (s) {
                return s + "foo";
            },
            timestamp: new Timestamp(1565545664, 1),
        },
        target: "string",
        expected: [
            '{"objectId":"507f1f77bcf86cd799439011",',
            '"uuid":"3b241101-e2bb-4255-8caf-4136c566a962",',
            '"date":"2018-03-27T16:58:51.538Z",',
            '"regex":"/^ABC/i",',
            '"js":"function (s) {\\n                return s + \\"foo\\";\\n            }",',
            '"timestamp":"Timestamp(1565545664, 1)"}',
        ].join(""),
    },
    {
        _id: 18,
        input: {int: 10, binData: BinData(4, "hn3f")},
        target: "string",
        expected: '{"int":10,"binData":"hn3f"}',
        // Ensure these are ignored.
        base: 16,
        format: "base64",
    },
    {
        _id: 19,
        input: {foo: [null, null]},
        target: "string",
        expected: '{"foo":[null,null]}',
        // onNull only applies on to the top-level expression. Hence it's ignored here.
        onNull: "on null!",
    },
    {
        _id: 20,
        input: nestedDepth140,
        target: "string",
        expected: JSON.stringify(nestedDepth140),
    },
    {
        _id: 21,
        // Value contains a null byte.
        input: {foo: "as\0df"},
        target: "string",
        expected: JSON.stringify({foo: "as\0df"}),
    },
    {
        _id: 22,
        input: {
            nan: NumberDecimal("NaN"),
            inf: NumberDecimal("-inf"),
            num: NumberDecimal("123.123"),
        },
        target: "string",
        expected: '{"nan":"NaN","inf":"-Infinity","num":123.123}',
    },
];

const oneMBString = "a".repeat(1 * 1024 * 1024);
const largeObject = {
    large: Object.fromEntries([
        // Fill up most of the document with 1mb strings.
        ...[...Array(15).keys()].map((i) => [(i + 1).toString(), oneMBString]),
        ["16", "a".repeat(1 * 1024 * 1024 - 512)],
        // These fit in the BSON size limit but will take more space when stringified, which will
        // cause the resulting string to go over the size limit.
        ...[...Array(24).keys()].map((i) => [(i + 16 + 1).toString(), -1234567891]),
    ]),
};
const illegalConversionTestDocs = [
    // Any type is allowed to be converted to string.
    // However, the result is not allowed to exceed the max BSON size.
    {_id: 23, input: largeObject, target: "string"},
];

runConvertTests({coll, requiresFCV83, conversionTestDocs, illegalConversionTestDocs});
