/**
 * Tests behavior of BinData $convert numeric.
 * @tags: [
 *   # BinData $convert numeric was added in v8.1.
 *   requires_fcv_81,
 * ]
 */

import {runConvertTests} from "jstests/libs/convert_shared.js";

const coll = db.expression_convert_bindata_numeric;
coll.drop();

const requiresFCV81 = true;

//
// Supported conversions.
//
const conversionTestDocs = [
    // Test conversions from BinData to Int.
    {
        _id: 0,
        // Hex: "0x0000002A", 4 byte integer
        input: BinData(0, "AAAAKg=="),
        target: "int",
        byteOrder: "big",
        expected: 42,
    },
    {
        _id: 1,
        // Hex: "0x=fe89", 2 byte integer
        input: BinData(0, "/ok="),
        target: "int",
        byteOrder: "big",
        expected: -375,
    },
    {
        _id: 2,
        // Hex: "0x=02", 1 byte integer
        input: BinData(0, "Ag=="),
        target: "int",
        byteOrder: "little",
        expected: 2,
    },
    // Test conversions from BinData to Long.
    {
        _id: 3,
        // Hex: "0x00000000000000c8", 8 byte long
        input: BinData(0, "AAAAAAAAAMg="),
        target: "long",
        byteOrder: "big",
        expected: 200,
    },
    {
        _id: 4,
        // Hex: "0xA2020000", 4 byte long
        input: BinData(0, "ogIAAA=="),
        target: "long",
        byteOrder: "little",
        expected: 674,
    },
    {
        _id: 5,
        // Hex: "0xc7cf", 2 byte long
        input: BinData(0, "x88="),
        target: "long",
        byteOrder: "little",
        expected: -12345,
    },
    {
        _id: 6,
        // Hex: "0xF4", 1 byte long
        input: BinData(0, "9A=="),
        target: "long",
        byteOrder: "big",
        expected: -12,
    },
    // Test conversions from Int to BinData.
    {
        _id: 7,
        input: NumberInt(-375),
        target: "binData",
        byteOrder: "big",
        // Hex: "0xfffffe89", 4 byte int
        expected: BinData(0, "///+iQ=="),
    },
    // Test conversions from Long to BinData
    {
        _id: 8,
        input: NumberLong(200),
        target: {type: "binData", subtype: 4},
        byteOrder: "big",
        // Hex: "0x00000000000000c8", 8 byte long
        expected: BinData(4, "AAAAAAAAAMg="),
    },
    // Test conversions from double to BinData
    {
        _id: 9,
        input: -2.5,
        target: "binData",
        byteOrder: "big",
        // Hex: "0xc004000000000000", 8 byte double
        expected: BinData(0, "wAQAAAAAAAA="),
    },
    {
        _id: 10,
        input: 813245678.98,
        target: {type: "binData", subtype: 3},
        byteOrder: "big",
        // Hex: "0x41C83C92777D70A4", 8 byte double
        expected: BinData(3, "Qcg8knd9cKQ="),
    },
    // Test conversions from BinData to double
    {
        _id: 11,
        // Hex: "0xc004000000000000", 8 byte double
        input: BinData(0, "wAQAAAAAAAA="),
        target: "double",
        byteOrder: "big",
        expected: -2.5,
    },
    {
        _id: 12,
        // Hex: "0x41533333", 4 byte single precision double
        input: BinData(0, "QVMzMw=="),
        target: "double",
        byteOrder: "big",
        expected: 13.199999809265137,
    },
    {
        _id: 13,
        // Hex: "0x000000000000E0BF", 8 byte double precision double
        input: BinData(5, "AAAAAAAA4L8="),
        target: "double",
        byteOrder: "little",
        expected: -0.5,
    },
    {
        _id: 14,
        // Hex: "0x0100807F", 4 byte single precision double
        input: BinData(0, "AQCAfw=="),
        target: "double",
        byteOrder: "little",
        expected: NaN,
    },
    {
        _id: 15,
        // Hex: "0xFFF0000000000000", 8 byte double precision inf
        input: BinData(0, "//AAAAAAAAA="),
        target: "double",
        byteOrder: "big",
        expected: -Infinity,
    },
    {
        _id: 16,
        // Hex: "0xC26CBF6CD2652666", 8 byte double precision inf
        input: BinData(2, "wmy/bNJlJmY="),
        target: "double",
        byteOrder: "big",
        expected: -987765314345.2,
    },
];

// Unsupported conversions.
//
const illegalConversionTestDocs = [
    // We don't support BinData to Decimal conversion.
    {
        _id: 17,
        input: NumberDecimal(1.5),
        target: "binData",
        byteOrder: "big",
    },
    // Wrong length of binary
    {
        _id: 18,
        input: BinData(0, "AAAAAAAAAMg="),
        target: "int",
        byteOrder: "big",
    },
    {
        _id: 19,
        // Hex: "0x0100807F00", 5 byte single precision double should error
        input: BinData(0, "AQCAfwA="),
        target: "double",
        byteOrder: "little",
    },
];

//
// Conversions with invalid 'to' argument.
//
const invalidTargetTypeDocs = [
    // Valid byteOrder is required when converting from / to BinData.
    {
        _id: 20,
        input: BinData(0, "AAAAKg=="),
        target: "int",
        // ByteOrder must either be 'little' or 'big'.
        byteOrder: "weird",
        expectedCode: 9130002,
    },
    {
        _id: 21,
        input: NumberLong(200),
        target: "binData",
        // ByteOrder must either be 'little' or 'big'.
        byteOrder: "silly",
        expectedCode: 9130002,
    },
    {
        _id: 22,
        input: NumberInt(200),
        target: "binData",
        // byteOrder must be a string.
        byteOrder: 1,
        expectedCode: 9130001,
    },
];

runConvertTests(
    {coll, requiresFCV81, conversionTestDocs, illegalConversionTestDocs, invalidTargetTypeDocs});

// Additional tests covering shortcuts and string byteOrder.
function testConvertNumeric({pipeline: convertPipeline, docs: documents}) {
    coll.drop();
    assert.commandWorked(coll.insertMany(documents));

    let aggResult = coll.aggregate(convertPipeline).toArray();
    aggResult.forEach(doc => {
        assert.eq(doc.output, doc.expected);
    });
}

(function testConvertBindataToLong() {
    let pipeline = [{
        $project: {
            _id: 0,
            expected: 1,
            output: {$convert: {to: "binData", input: "$longInput", byteOrder: "big"}}
        }
    }];
    testConvertNumeric({
        pipeline: pipeline,
        // Hex: "0x00000000000000c8", 8 byte long
        docs: [{longInput: NumberLong(200), expected: BinData(0, "AAAAAAAAAMg=")}]
    });
})();

(function testConvertBindataToInt() {
    let pipeline = [{
        $project: {
            _id: 0,
            expected: 1,
            output: {$convert: {to: "binData", input: "$IntInput", byteOrder: "big"}}
        }
    }];
    testConvertNumeric({
        pipeline: pipeline,
        // Hex: "0xfffffe89", 4 byte int
        docs: [{IntInput: NumberInt(-375), expected: BinData(0, "///+iQ==")}]
    });
})();

(function testConvertBindataToDouble() {
    let pipeline = [{
        $project: {
            _id: 0,
            expected: 1,
            output: {$convert: {to: "binData", input: "$DoubleInput", byteOrder: "big"}}
        }
    }];
    testConvertNumeric({
        pipeline: pipeline,
        // Hex: "0xC004CCCCCCCCCCCD", 8 byte double precision double
        docs: [{DoubleInput: -2.6, expected: BinData(0, "wATMzMzMzM0=")}]
    });
})();

(function testConvertBindataToIntShortCut() {
    let pipeline = [{$project: {_id: 0, expected: 1, output: {$toInt: "$binDataInput"}}}];
    // Hex: "0x=02", 1 byte integer
    testConvertNumeric(
        {pipeline: pipeline, docs: [{binDataInput: BinData(0, "Ag=="), expected: NumberInt(2)}]});
})();

(function testConvertBindataToLongShortCut() {
    let pipeline = [{$project: {_id: 0, expected: 1, output: {$toLong: "$binDataInput"}}}];
    testConvertNumeric({
        pipeline: pipeline,
        // Hex: "0xA2020000", 4 byte long
        docs: [{binDataInput: BinData(6, "ogIAAA=="), expected: NumberLong(674)}]
    });
})();

(function testConvertBindataToDoubleShortCut() {
    let pipeline = [{$project: {_id: 0, expected: 1, output: {$toDouble: "$binDataInput"}}}];
    testConvertNumeric({
        pipeline: pipeline,
        // Hex: "0xCDCCCCCCCCCC04C0", 4 byte long
        docs: [{binDataInput: BinData(0, "zczMzMzMBMA="), expected: -2.6}]
    });
})();
