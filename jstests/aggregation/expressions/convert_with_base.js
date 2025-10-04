/**
 * Tests behavior of BinData $convert.
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {runConvertTests} from "jstests/libs/query/convert_shared.js";

const coll = db.expression_convert_with_base;
coll.drop();

const requiresFCV83 = true;

const conversionTestDocs = [
    // Convert number to string.
    {_id: 1, input: NumberInt(10), target: "string", base: 2, expected: "1010"},
    {_id: 2, input: 10.0, target: "string", base: 2, expected: "1010"},
    {_id: 3, input: 10, target: "string", base: 2, expected: "1010"},
    {_id: 4, input: NumberLong(10), target: "string", base: 2, expected: "1010"},
    {_id: 5, input: NumberDecimal(10.0), target: "string", base: 2, expected: "1010"},

    {_id: 6, input: NumberInt(10), target: "string", base: 8, expected: "12"},
    {_id: 7, input: 10.0, target: "string", base: 8, expected: "12"},
    {_id: 8, input: 10, target: "string", base: 8, expected: "12"},
    {_id: 9, input: NumberLong(10), target: "string", base: 8, expected: "12"},
    {_id: 10, input: NumberDecimal(10.0), target: "string", base: 8, expected: "12"},

    {_id: 11, input: NumberInt(10), target: "string", base: 10, expected: "10"},
    {_id: 12, input: 10.0, target: "string", base: 10, expected: "10"},
    {_id: 13, input: 10, target: "string", base: 10, expected: "10"},
    {_id: 14, input: NumberLong(10), target: "string", base: 10, expected: "10"},
    {_id: 15, input: NumberDecimal(10.0), target: "string", base: 10, expected: "10"},

    {_id: 16, input: NumberInt(10), target: "string", base: 16, expected: "A"},
    {_id: 17, input: 10.0, target: "string", base: 16, expected: "A"},
    {_id: 18, input: 10, target: "string", base: 16, expected: "A"},
    {_id: 19, input: NumberLong(10), target: "string", base: 16, expected: "A"},
    {_id: 20, input: NumberDecimal(10.0), target: "string", base: 16, expected: "A"},

    // Convert number to string with negative input.
    {_id: 21, input: NumberInt(-10), target: "string", base: 2, expected: "-1010"},
    {_id: 22, input: -10, target: "string", base: 8, expected: "-12"},
    {_id: 23, input: NumberLong(-10), target: "string", base: 10, expected: "-10"},
    {_id: 24, input: NumberDecimal("-10"), target: "string", base: 16, expected: "-A"},

    // Convert string to number.
    {_id: 25, input: "1010", target: "int", base: 2, expected: NumberInt(10)},
    {_id: 26, input: "1010", target: "double", base: 2, expected: 10.0},
    {_id: 27, input: "1010", target: "long", base: 2, expected: NumberLong(10)},
    {_id: 28, input: "1010", target: "decimal", base: 2, expected: NumberDecimal("10")},

    {_id: 29, input: "12", target: "int", base: 8, expected: NumberInt(10)},
    {_id: 30, input: "12", target: "double", base: 8, expected: 10.0},
    {_id: 31, input: "12", target: "long", base: 8, expected: NumberLong(10)},
    {_id: 32, input: "12", target: "decimal", base: 8, expected: NumberDecimal("10")},

    {_id: 33, input: "10", target: "int", base: 10, expected: NumberInt(10)},
    {_id: 34, input: "10", target: "double", base: 10, expected: 10.0},
    {_id: 35, input: "10", target: "long", base: 10, expected: NumberLong(10)},
    {_id: 36, input: "10", target: "decimal", base: 10, expected: NumberDecimal("10")},

    {_id: 37, input: "A", target: "int", base: 16, expected: NumberInt(10)},
    {_id: 38, input: "A", target: "double", base: 16, expected: 10.0},
    {_id: 39, input: "A", target: "long", base: 16, expected: NumberLong(10)},
    {_id: 40, input: "A", target: "decimal", base: 16, expected: NumberDecimal("10")},
    {_id: 41, input: "a", target: "int", base: 16, expected: NumberInt(10)},
    {_id: 42, input: "a", target: "double", base: 16, expected: 10.0},
    {_id: 43, input: "a", target: "long", base: 16, expected: NumberLong(10)},
    {_id: 44, input: "a", target: "decimal", base: 16, expected: NumberDecimal("10")},

    // Convert string to number with negative input.
    {_id: 45, input: "-1010", target: "int", base: 2, expected: NumberInt(-10)},
    {_id: 46, input: "-12", target: "double", base: 8, expected: -10.0},
    {_id: 47, input: "-10", target: "long", base: 10, expected: NumberLong(-10)},
    {_id: 48, input: "-A", target: "decimal", base: 16, expected: NumberDecimal("-10")},

    // Base param is validated but ignored when conversion is not between string and number.
    {_id: 49, input: 1.9, target: "double", base: 2, expected: 1.9},
    {_id: 50, input: 1.9, target: "date", base: 2, expected: ISODate("1970-01-01T00:00:00.001Z")},
    {_id: 51, input: "str", target: "string", base: 2, expected: "str"},
    {
        _id: 52,
        input: "0123456789abcdef01234567",
        target: "objectId",
        base: 2,
        expected: ObjectId("0123456789abcdef01234567"),
    },
    {_id: 53, input: ObjectId("0123456789abcdef01234567"), target: "bool", expected: true},
    {
        _id: 54,
        input: ObjectId("0123456789abcdef01234567"),
        target: "objectId",
        base: 2,
        expected: ObjectId("0123456789abcdef01234567"),
    },
    {_id: 55, input: false, target: "double", base: 2, expected: 0.0},
    {_id: 56, input: false, target: "bool", base: 2, expected: false},
    {
        _id: 57,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "date",
        base: 2,
        expected: ISODate("1970-01-01T00:00:00.123Z"),
    },
    {
        _id: 58,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "long",
        base: 2,
        expected: NumberLong(123),
    },
    {_id: 59, input: NumberInt(1), target: "int", base: 2, expected: NumberInt(1)},
    {_id: 60, input: NumberInt(1), target: "long", base: 2, expected: NumberLong(1)},
    {_id: 61, input: NumberLong(1), target: "double", base: 2, expected: 1.0},
    {_id: 62, input: NumberLong(1), target: "long", base: 2, expected: NumberLong(1)},
    {_id: 63, input: NumberDecimal("1.9"), target: "double", base: 2, expected: 1.9},
    {_id: 64, input: NumberDecimal("1.9"), target: "bool", base: 2, expected: true},
    {
        _id: 65,
        input: NumberDecimal("1.9"),
        target: "decimal",
        base: 2,
        expected: NumberDecimal("1.9"),
    },

    // Complementary examples of illegal conversions.
    {_id: 66, input: "A", target: "int", base: 16, expected: NumberInt(10)},

    {_id: 67, input: "8", target: "double", base: 10, expected: 8.0},
    {_id: 68, input: "8", target: "long", base: 16, expected: NumberLong(8)},

    {_id: 69, input: "2", target: "decimal", base: 8, expected: NumberDecimal("2")},
    {_id: 70, input: "2", target: "int", base: 10, expected: NumberInt(2)},
    {_id: 71, input: "2", target: "double", base: 16, expected: 2},

    // Base null equals empty base.
    {_id: 72, input: NumberInt(10), target: "string", base: null, expected: "10"},
    {_id: 73, input: 10, target: "string", base: null, expected: "10"},
    {_id: 74, input: NumberLong(10), target: "string", base: null, expected: "10"},
    {_id: 75, input: NumberDecimal("10"), target: "string", base: null, expected: "10"},

    {_id: 76, input: "10", target: "int", base: null, expected: NumberInt(10)},
    {_id: 77, input: "10", target: "double", base: null, expected: 10.0},
    {_id: 78, input: "10", target: "long", base: null, expected: NumberLong(10)},
    {_id: 79, input: "10", target: "decimal", base: null, expected: NumberDecimal("10")},

    // Conversion from number to string with large numbers.
    {
        _id: 80,
        input: NumberInt(2147483647),
        target: "string",
        base: 2,
        expected: "1111111111111111111111111111111",
    },
    {
        _id: 81,
        input: NumberInt(-2147483648),
        target: "string",
        base: 2,
        expected: "-10000000000000000000000000000000",
    },
    {
        _id: 82,
        input: NumberLong("9223372036854775807"),
        target: "string",
        base: 2,
        expected: "111111111111111111111111111111111111111111111111111111111111111",
    },
    {
        _id: 83,
        input: NumberLong("-9223372036854775808"),
        target: "string",
        base: 2,
        expected: "-1000000000000000000000000000000000000000000000000000000000000000",
    },

    {_id: 84, input: NumberInt(2147483647), target: "string", base: 8, expected: "17777777777"},
    {_id: 85, input: NumberInt(-2147483648), target: "string", base: 8, expected: "-20000000000"},
    {
        _id: 86,
        input: NumberLong("9223372036854775807"),
        target: "string",
        base: 8,
        expected: "777777777777777777777",
    },
    {
        _id: 87,
        input: NumberLong("-9223372036854775808"),
        target: "string",
        base: 8,
        expected: "-1000000000000000000000",
    },

    {_id: 88, input: NumberInt(2147483647), target: "string", base: 10, expected: "2147483647"},
    {_id: 89, input: NumberInt(-2147483648), target: "string", base: 10, expected: "-2147483648"},
    {
        _id: 90,
        input: NumberLong("9223372036854775807"),
        target: "string",
        base: 10,
        expected: "9223372036854775807",
    },
    {
        _id: 91,
        input: NumberLong("-9223372036854775808"),
        target: "string",
        base: 10,
        expected: "-9223372036854775808",
    },

    {_id: 92, input: NumberInt(2147483647), target: "string", base: 16, expected: "7FFFFFFF"},
    {_id: 93, input: NumberInt(-2147483648), target: "string", base: 16, expected: "-80000000"},
    {
        _id: 94,
        input: NumberLong("9223372036854775807"),
        target: "string",
        base: 16,
        expected: "7FFFFFFFFFFFFFFF",
    },
    {
        _id: 95,
        input: NumberLong("-9223372036854775808"),
        target: "string",
        base: 16,
        expected: "-8000000000000000",
    },
];

//
// Unsupported conversions.
//
const illegalConversionTestDocs = [
    // Given string is not valid in the base.
    {_id: 1, input: "zzz", target: "int", base: 2},
    {_id: 2, input: "zzz", target: "double", base: 8},
    {_id: 3, input: "zzz", target: "long", base: 10},
    {_id: 4, input: "zzz", target: "decimal", base: 16},

    {_id: 5, input: "A", target: "int", base: 2},
    {_id: 6, input: "A", target: "double", base: 8},
    {_id: 7, input: "A", target: "long", base: 10},

    {_id: 8, input: "8", target: "decimal", base: 2},
    {_id: 9, input: "8", target: "int", base: 8},

    {_id: 10, input: "2", target: "double", base: 2},

    // Trying the convert a non-integral number using base.
    {_id: 11, input: 10.1, target: "string", base: 2},
    {_id: 12, input: NumberDecimal(10.1), target: "string", base: 8},
    {_id: 13, input: 10.1, target: "string", base: 10},
    {_id: 14, input: NumberDecimal(10.1), target: "string", base: 16},

    // Number represented by given string is too large to fit in number.
    {_id: 15, input: "10000000000000000000000000000000", target: "int", base: 2},
    {
        _id: 16,
        input: "1000000000000000000000000000000000000000000000000000000000000000",
        target: "double",
        base: 2,
    },
    {
        _id: 17,
        input: "1000000000000000000000000000000000000000000000000000000000000000",
        target: "long",
        base: 2,
    },
    {
        _id: 18,
        input: "1000000000000000000000000000000000000000000000000000000000000000",
        target: "decimal",
        base: 2,
    },
    {_id: 19, input: "-10000000000000000000000000000001", target: "int", base: 2},
    {
        _id: 20,
        input: "-1000000000000000000000000000000000000000000000000000000000000001",
        target: "double",
        base: 2,
    },
    {
        _id: 21,
        input: "-1000000000000000000000000000000000000000000000000000000000000001",
        target: "long",
        base: 2,
    },
    {
        _id: 22,
        input: "-1000000000000000000000000000000000000000000000000000000000000001",
        target: "decimal",
        base: 2,
    },

    {_id: 23, input: "20000000000", target: "int", base: 8},
    {_id: 24, input: "1000000000000000000000", target: "double", base: 8},
    {_id: 25, input: "1000000000000000000000", target: "long", base: 8},
    {_id: 26, input: "1000000000000000000000", target: "decimal", base: 8},
    {_id: 27, input: "-20000000001", target: "int", base: 8},
    {_id: 28, input: "-1000000000000000000001", target: "double", base: 8},
    {_id: 29, input: "-1000000000000000000001", target: "long", base: 8},
    {_id: 30, input: "-1000000000000000000001", target: "decimal", base: 8},

    {_id: 31, input: "2147483648", target: "int", base: 10},
    {_id: 32, input: "9223372036854775808", target: "double", base: 10},
    {_id: 33, input: "9223372036854775808", target: "long", base: 10},
    {_id: 34, input: "9223372036854775808", target: "decimal", base: 10},
    {_id: 35, input: "-2147483649", target: "int", base: 10},
    {_id: 36, input: "-9223372036854775809", target: "double", base: 10},
    {_id: 37, input: "-9223372036854775809", target: "long", base: 10},
    {_id: 38, input: "-9223372036854775809", target: "decimal", base: 10},

    {_id: 39, input: "80000000", target: "int", base: 16},
    {_id: 40, input: "8000000000000000", target: "double", base: 16},
    {_id: 41, input: "8000000000000000", target: "long", base: 16},
    {_id: 42, input: "8000000000000000", target: "decimal", base: 16},
    {_id: 43, input: "-80000001", target: "int", base: 16},
    {_id: 44, input: "-8000000000000001", target: "double", base: 16},
    {_id: 45, input: "-8000000000000001", target: "long", base: 16},
    {_id: 46, input: "-8000000000000001", target: "decimal", base: 16},

    // Reject conversions with base prefix
    {_id: 47, input: "0b1010", target: "int", base: 2},
    {_id: 48, input: "0B1010", target: "double", base: 2},
    {_id: 49, input: "0o12", target: "long", base: 8},
    {_id: 50, input: "0O12", target: "decimal", base: 8},
    {_id: 53, input: "0xa", target: "long", base: 16},
    {_id: 54, input: "0XA", target: "decimal", base: 16},
];

//
// Conversions with invalid 'base' argument.
//
const invalidArgumentValueDocs = [
    // Base must be integral.
    {_id: 1, input: NumberInt(10), target: "string", base: 2.1, expectedCode: 3501300},
    {_id: 2, input: 10, target: "string", base: 2.1, expectedCode: 3501300},
    {_id: 3, input: NumberLong(10), target: "string", base: 2.1, expectedCode: 3501300},
    {_id: 4, input: NumberDecimal(10.0), target: "string", base: 2.1, expectedCode: 3501300},

    {_id: 5, input: "1010", target: "int", base: 2.1, expectedCode: 3501300},
    {_id: 6, input: "1010", target: "double", base: 2.1, expectedCode: 3501300},
    {_id: 7, input: "1010", target: "long", base: 2.1, expectedCode: 3501300},
    {_id: 8, input: "1010", target: "decimal", base: 2.1, expectedCode: 3501300},

    // Base must be 2, 8, 10 or 16
    {_id: 9, input: NumberInt(10), target: "string", base: 3, expectedCode: 3501301},
    {_id: 10, input: 10, target: "string", base: "invalid", expectedCode: 3501300},
    {_id: 11, input: NumberLong(10), target: "string", base: 0, expectedCode: 3501301},
    {_id: 12, input: NumberDecimal(10.0), target: "string", base: 12, expectedCode: 3501301},

    {_id: 13, input: "1010", target: "int", base: 3, expectedCode: 3501301},
    {_id: 14, input: "1010", target: "double", base: "invalid", expectedCode: 3501300},
    {_id: 15, input: "1010", target: "long", base: 0, expectedCode: 3501301},
    {_id: 16, input: "1010", target: "decimal", base: 12, expectedCode: 3501301},

    // Base is validated, even when it's not used.
    {_id: 17, input: 1.9, target: "double", base: 2.1, expectedCode: 3501300},
    {_id: 19, input: "str", target: "string", base: 3, expectedCode: 3501301},
    {
        _id: 54,
        input: ObjectId("0123456789abcdef01234567"),
        target: "objectId",
        base: 2.1,
        expectedCode: 3501300,
    },
    {_id: 56, input: false, target: "bool", base: 3, expectedCode: 3501301},
    {
        _id: 57,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "date",
        base: 2.1,
        expectedCode: 3501300,
    },
    {_id: 59, input: NumberInt(1), target: "int", base: 3, expectedCode: 3501301},
    {_id: 62, input: NumberLong(1), target: "long", base: 2.1, expectedCode: 3501300},
    {_id: 63, input: NumberDecimal("1.9"), target: "decimal", base: 3, expectedCode: 3501301},
    {_id: 65, input: NumberDecimal("1.9"), target: "decimal", base: 2.1, expectedCode: 3501300},
];

runConvertTests({
    coll,
    requiresFCV83,
    runShorthandTests: false,
    runRoundTripTests: true,
    conversionTestDocs,
    illegalConversionTestDocs,
    invalidArgumentValueDocs,
});
