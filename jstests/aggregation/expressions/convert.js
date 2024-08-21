/**
 * Tests behavior of $convert aggregation operator.
 */

import {runConvertTests} from "jstests/libs/convert_shared.js";

const coll = db.expression_convert;
coll.drop();

const requiresFCV80 = false;

//
// One test document for each possible conversion. Edge cases for these conversions are tested
// in expression_convert_test.cpp.
//
const conversionTestDocs = [
    {_id: 0, input: 1.9, target: "double", expected: 1.9},
    {_id: 1, input: 1.9, target: "string", expected: "1.9"},
    {_id: 2, input: 1.9, target: "bool", expected: true},
    {_id: 3, input: 1.9, target: "date", expected: ISODate("1970-01-01T00:00:00.001Z")},
    {_id: 4, input: 1.9, target: "int", expected: NumberInt(1)},
    {_id: 5, input: 1.9, target: "long", expected: NumberLong(1)},
    {_id: 6, input: 1.9, target: "decimal", expected: NumberDecimal(1.9)},

    {_id: 7, input: "1.9", target: "double", expected: 1.9},
    {_id: 8, input: "str", target: "string", expected: "str"},
    {
        _id: 9,
        input: "0123456789abcdef01234567",
        target: "objectId",
        expected: ObjectId("0123456789abcdef01234567")
    },
    {_id: 10, input: "", target: "bool", expected: true},
    {
        _id: 11,
        input: "1970-01-01T00:00:00.001Z",
        target: "date",
        expected: ISODate("1970-01-01T00:00:00.001Z")
    },
    {_id: 12, input: "1", target: "int", expected: NumberInt(1)},
    {_id: 13, input: "1", target: "long", expected: NumberLong(1)},
    {_id: 14, input: "1.9", target: "decimal", expected: NumberDecimal("1.9")},

    {
        _id: 15,
        input: ObjectId("0123456789abcdef01234567"),
        target: "string",
        expected: "0123456789abcdef01234567"
    },
    {_id: 16, input: ObjectId("0123456789abcdef01234567"), target: "bool", expected: true},
    {
        _id: 17,
        input: ObjectId("0123456789abcdef01234567"),
        target: "objectId",
        expected: ObjectId("0123456789abcdef01234567")
    },
    {
        _id: 18,
        input: ObjectId("0123456789abcdef01234567"),
        target: "date",
        expected: ISODate("1970-08-09T22:25:43Z")
    },

    {_id: 19, input: false, target: "double", expected: 0.0},
    {_id: 20, input: false, target: "string", expected: "false"},
    {_id: 21, input: false, target: "bool", expected: false},
    {_id: 22, input: false, target: "int", expected: NumberInt(0)},
    {_id: 23, input: false, target: "long", expected: NumberLong(0)},
    {_id: 24, input: false, target: "decimal", expected: NumberDecimal(0)},

    {_id: 25, input: ISODate("1970-01-01T00:00:00.123Z"), target: "double", expected: 123.0},
    {
        _id: 26,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "string",
        expected: "1970-01-01T00:00:00.123Z"
    },
    {_id: 27, input: ISODate("1970-01-01T00:00:00.123Z"), target: "bool", expected: true},
    {
        _id: 28,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "date",
        expected: ISODate("1970-01-01T00:00:00.123Z")
    },
    {
        _id: 29,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "long",
        expected: NumberLong(123)
    },
    {
        _id: 30,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "decimal",
        expected: NumberDecimal("123")
    },

    {_id: 31, input: NumberInt(1), target: "double", expected: 1.0},
    {_id: 32, input: NumberInt(1), target: "string", expected: "1"},
    {_id: 33, input: NumberInt(1), target: "bool", expected: true},
    {_id: 34, input: NumberInt(1), target: "int", expected: NumberInt(1)},
    {_id: 35, input: NumberInt(1), target: "long", expected: NumberLong(1)},
    {_id: 36, input: NumberInt(1), target: "decimal", expected: NumberDecimal("1")},

    {_id: 37, input: NumberLong(1), target: "double", expected: 1.0},
    {_id: 38, input: NumberLong(1), target: "string", expected: "1"},
    {_id: 39, input: NumberLong(1), target: "bool", expected: true},
    {_id: 40, input: NumberLong(1), target: "date", expected: ISODate("1970-01-01T00:00:00.001Z")},
    {_id: 41, input: NumberLong(1), target: "int", expected: NumberInt(1)},
    {_id: 42, input: NumberLong(1), target: "long", expected: NumberLong(1)},
    {_id: 43, input: NumberLong(1), target: "decimal", expected: NumberDecimal("1")},

    {_id: 44, input: NumberDecimal("1.9"), target: "double", expected: 1.9},
    {_id: 45, input: NumberDecimal("1.9"), target: "string", expected: "1.9"},
    {_id: 46, input: NumberDecimal("1.9"), target: "bool", expected: true},
    {
        _id: 47,
        input: NumberDecimal("1.9"),
        target: "date",
        expected: ISODate("1970-01-01T00:00:00.001Z")
    },
    {_id: 48, input: NumberDecimal("1.9"), target: "int", expected: NumberInt(1)},
    {_id: 49, input: NumberDecimal("1.9"), target: "long", expected: NumberLong(1)},
    {_id: 50, input: NumberDecimal("1.9"), target: "decimal", expected: NumberDecimal("1.9")},

    {_id: 51, input: MinKey, target: "bool", expected: true},
    {_id: 52, input: {foo: 1, bar: 2}, target: "bool", expected: true},
    {_id: 53, input: [1, 2], target: "bool", expected: true},
    {_id: 54, input: BinData(0, "BBBBBBBBBBBBBBBBBBBBBBBBBBBB"), target: "bool", expected: true},
    {_id: 55, input: /B*/, target: "bool", expected: true},
    {_id: 56, input: new DBRef("db.test", "oid"), target: "bool", expected: true},
    {_id: 57, input: function() {}, target: "bool", expected: true},
    // Symbol and CodeWScope are not supported from JavaScript, so we can't test them here.
    {_id: 58, input: new Timestamp(1 / 1000, 1), target: "bool", expected: true},
    {_id: 59, input: MinKey, target: "bool", expected: true},

    {
        _id: 60,
        input: Timestamp(1, 1),
        target: "date",
        expected: ISODate("1970-01-01T00:00:01.000Z")
    },
];

//
// Unsupported conversions.
//
const illegalConversionTestDocs = [
    {_id: 0, input: 1.9, target: "objectId"},

    {_id: 1, input: ObjectId("0123456789abcdef01234567"), target: "double"},
    {_id: 2, input: ObjectId("0123456789abcdef01234567"), target: "int"},
    {_id: 3, input: ObjectId("0123456789abcdef01234567"), target: "long"},
    {_id: 4, input: ObjectId("0123456789abcdef01234567"), target: "decimal"},

    {_id: 5, input: false, target: "objectId"},
    {_id: 6, input: false, target: "date"},

    {_id: 7, input: ISODate("1970-01-01T00:00:00.123Z"), target: "objectId"},
    {_id: 8, input: ISODate("1970-01-01T00:00:00.123Z"), target: "int"},

    {_id: 9, input: NumberInt(1), target: "objectId"},
    {_id: 10, input: NumberInt(1), target: "date"},

    {_id: 11, input: NumberLong(1), target: "objectId"},

    {_id: 12, input: NumberDecimal("1.9"), target: "objectId"},

    {_id: 13, input: 1.9, target: "minKey"},
    {_id: 14, input: 1.9, target: "missing"},
    {_id: 15, input: 1.9, target: "object"},
    {_id: 16, input: 1.9, target: "array"},
    {_id: 18, input: 1.9, target: "undefined"},
    {_id: 19, input: 1.9, target: "null"},
    {_id: 20, input: 1.9, target: "regex"},
    {_id: 21, input: 1.9, target: "dbPointer"},
    {_id: 22, input: 1.9, target: "javascript"},
    {_id: 23, input: 1.9, target: "symbol"},
    {_id: 24, input: 1.9, target: "javascriptWithScope"},
    {_id: 25, input: 1.9, target: "timestamp"},
    {_id: 26, input: 1.9, target: "maxKey"},
];

//
// One test document for each "nullish" value.
//
const nullTestDocs =
    [{_id: 0, input: null}, {_id: 1, input: undefined}, {_id: 2, /* input is missing */}];

runConvertTests({coll, requiresFCV80, conversionTestDocs, illegalConversionTestDocs, nullTestDocs});
