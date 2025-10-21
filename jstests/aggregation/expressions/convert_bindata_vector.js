/**
 * Tests behavior of BinData $convert array.
 * @tags: [
 *   # BinData $convert array was added in v8.3.
 *   requires_fcv_83,
 *   featureFlagConvertBinDataVectors,
 * ]
 */

// Specification is taken from
// https://github.com/mongodb/specifications/blob/9d0d3f0042a8cf5faeb47ae7765716151bfca9ef/source/bson-binary-vector/bson-binary-vector.md#data-types-dtypes.
const kBindataVectorSubtype = 9;
const kInt8Byte = "03";
const kFloat32Byte = "27";
const kPackedBitByte = "10";

/**
 * Test cases that should succeed in being converted both directions (from bindata vector to BSON
 * array and vice versa).
 *
 * Padding is ignored unless the dtype is kPackedBitByte.
 * Special cases (such as internal conversions to different dtypes) are handled by the test
 * infrastructure and noted in the test cases below.
 */
let testCases = [
    {array_elems: [], padding: 0, dtype: kPackedBitByte},
    {array_elems: [127, 7], padding: 0, dtype: kPackedBitByte},
    {array_elems: [238, 224], padding: 4, dtype: kPackedBitByte},
    {array_elems: [128, 8], padding: 3, dtype: kPackedBitByte},
    // Note that the following converts from bson->bindata as an empty array of packed bits.
    {array_elems: [], padding: 0, dtype: kInt8Byte},
    // Note that the following converts from bson->bindata as an array of 1 packed bit.
    {array_elems: [0], padding: 0, dtype: kInt8Byte},
    // Note that the following converts from bson->bindata as an array of packed bits.
    {array_elems: [0, 1], padding: 0, dtype: kInt8Byte},
    {array_elems: [0, 1, 0, 10], padding: 0, dtype: kInt8Byte},
    // Note that the following converts from bson->bindata as an array of packed bits.
    {array_elems: [0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1], padding: 0, dtype: kInt8Byte},
    {array_elems: [2], padding: 0, dtype: kInt8Byte},
    {array_elems: [127, 7], padding: 0, dtype: kInt8Byte},
    // Note that the following converts from bson->bindata as an empty array of packed bits.
    {array_elems: [], padding: 0, dtype: kFloat32Byte},
    {array_elems: [0.3], padding: 0, dtype: kFloat32Byte},
    {array_elems: [1.2], padding: 0, dtype: kFloat32Byte},
    {array_elems: [2.2], padding: 0, dtype: kFloat32Byte},
    // Note that the following converts from bson->bindata as an array of int8s.
    {array_elems: [127.0, 7.0, -128.0], padding: 0, dtype: kFloat32Byte},
    {array_elems: [128.0, 7.0], padding: 0, dtype: kFloat32Byte},
    {array_elems: [-129.0, 7.0], padding: 0, dtype: kFloat32Byte},
    {array_elems: [-127.7, -7.7], padding: 0, dtype: kFloat32Byte},
    {
        array_elems: [Number.NEGATIVE_INFINITY, 0.0, Number.POSITIVE_INFINITY],
        padding: 0,
        dtype: kFloat32Byte,
    },

    // Big-endian versions of the above test cases.
    {array_elems: [], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [0.3], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [1.2], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [2.2], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [127.0, 7.0, -128.0], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [128.0, 7.0], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [-129.0, 7.0], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {array_elems: [-127.7, -7.7], padding: 0, dtype: kFloat32Byte, littleEndian: false},
    {
        array_elems: [Number.NEGATIVE_INFINITY, 0.0, Number.POSITIVE_INFINITY],
        padding: 0,
        dtype: kFloat32Byte,
        littleEndian: false,
    },

    // TODO SERVER-106059 Add tests for integers larger than INT8.
];

function int8VectorToBitArray(vector) {
    const bitArray = [];

    for (const int8 of vector) {
        const byte = (int8 < 0 ? 256 + int8 : int8).toString(2).padStart(8, "0");
        bitArray.push(...byte.split("").map((bit) => parseInt(bit, 10)));
    }

    return bitArray;
}

function hexToBitArray(hexString) {
    if (hexString.length % 2 !== 0) {
        throw new Error("Invalid hex string. Length must be even.");
    }

    const bitArray = [];

    for (let i = 0; i < hexString.length; i += 2) {
        const byte = parseInt(hexString.substr(i, 2), 16);
        const bits = byte.toString(2).padStart(8, "0");
        bitArray.push(...bits.split("").map((bit) => parseInt(bit, 10)));
    }

    return bitArray;
}

function float32VectorToBitArray(vector, littleEndian = true) {
    const bitArray = [];

    for (const value of vector) {
        const buffer = new ArrayBuffer(4); // 4 bytes for float32
        const view = new DataView(buffer);
        view.setFloat32(0, value, littleEndian);

        for (let i = 0; i < 4; i++) {
            const byte = view.getUint8(i);
            const bits = byte.toString(2).padStart(8, "0");
            bitArray.push(...bits.split("").map((bit) => parseInt(bit, 10)));
        }
    }

    return bitArray;
}

function bitArrayToBoolArray(vector) {
    let ret = [];
    for (var b of vector) {
        ret.push(b == "1");
    }
    return ret;
}

function bytesToBase64(byteArray) {
    const base64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let result = "";
    let i;

    for (i = 0; i < byteArray.length; i += 3) {
        let chunk = (byteArray[i] << 16) | ((byteArray[i + 1] || 0) << 8) | (byteArray[i + 2] || 0);

        result += base64chars[(chunk >> 18) & 63];
        result += base64chars[(chunk >> 12) & 63];
        result += i + 1 < byteArray.length ? base64chars[(chunk >> 6) & 63] : "=";
        result += i + 2 < byteArray.length ? base64chars[chunk & 63] : "=";
    }

    return result;
}

function bitArrayToByteArray(bitArray) {
    if (!Array.isArray(bitArray) || !bitArray.every((bit) => bit == 0 || bit == 1)) {
        throw new Error("Input must be an array of 0s and 1s.");
    }

    // Pad bit array to multiple of 8
    const padded = [...bitArray];
    while (padded.length % 8 !== 0) padded.push(0);

    // Convert to byte array
    const byteArray = new Uint8Array(padded.length / 8);
    for (let i = 0; i < byteArray.length; i++) {
        let byte = 0;
        for (let bit = 0; bit < 8; bit++) {
            byte = (byte << 1) | padded[i * 8 + bit];
        }
        byteArray[i] = byte;
    }

    return byteArray;
}

function bitArrayToBase64String(bitArray) {
    let byteArray = bitArrayToByteArray(bitArray);
    return bytesToBase64(byteArray);
}

/**
 * Create a bindata vector bit array.
 * @param {string} dataTypeByte: hex string representing the dtype, taken from bindata vector
 *     specification
 * @param {array<number>} vector: a vector of int8 or float32s, containing the values of the array
 * @param {number} numPaddingBits: the number of padding bits, applicable in the PACKED_BIT case
 */
function createBindataVectorBitArray(dataTypeByte, vector, numPaddingBits, littleEndian = true) {
    let dTypeBitArray = hexToBitArray(dataTypeByte);
    let paddingBitArray =
        dataTypeByte == kPackedBitByte ? int8VectorToBitArray([numPaddingBits]) : int8VectorToBitArray([0]);
    let arrayElemsBitArray =
        dataTypeByte == kFloat32Byte ? float32VectorToBitArray(vector, littleEndian) : int8VectorToBitArray(vector);
    return [...dTypeBitArray, ...paddingBitArray, ...arrayElemsBitArray];
}

testCases.forEach((testCase) => {
    let {dtype, array_elems, padding, littleEndian = true} = testCase;

    // Determine base64-encoded version of bindata vector, and the bson array of the vector.
    let bindataArray = createBindataVectorBitArray(dtype, array_elems, padding, littleEndian);
    let base64BindataArray = bitArrayToBase64String(bindataArray);
    let bsonArray = array_elems;
    if (dtype == kPackedBitByte) {
        bsonArray = bitArrayToBoolArray(int8VectorToBitArray(bsonArray));
        if (padding > 0) {
            bsonArray = bsonArray.slice(0, -1 * padding);
        }
    } else if (dtype == kInt8Byte) {
        // Javascript types default to doubles in BSON, so we must explicitly cast them to ints in
        // this case.
        bsonArray = bsonArray.map((n) => NumberInt(n));
    }

    // Insert a doc containing the test case into the collection.
    let doc = {
        _id: 0,
        bson_array: bsonArray,
        bindata_array_base64: BinData(kBindataVectorSubtype, base64BindataArray),
        approx: dtype == kFloat32Byte, // BSON does not support FLOAT32, so results may not be exact.
    };
    const coll = db.expression_convert_bindata_vector;
    coll.drop();
    assert.commandWorked(coll.insertMany([doc]));

    // Verify conversion from bindata vector to BSON array.
    let bindataToBsonPipeline = [
        {
            $project: {
                _id: 0,
                approx: 1,
                expected: "$bson_array",
                output: {
                    $convert: {
                        to: {type: "array"},
                        input: "$bindata_array_base64",
                        byteOrder: littleEndian ? "little" : "big",
                    },
                },
            },
        },
    ];
    let bindataToBsonResult = coll.aggregate(bindataToBsonPipeline).toArray();
    bindataToBsonResult.forEach((doc) => {
        if (doc.approx) {
            // assert.close() does not work on arrays so manually compare each value.
            assert.eq(doc.output.length, doc.expected.length);
            for (let i = 0; i < doc.output.length; i++) {
                if (doc.output[i] == Number.NEGATIVE_INFINITY || doc.output[i] == Number.POSITIVE_INFINITY) {
                    assert.eq(doc.output[i], doc.expected[i]);
                } else {
                    assert.close(doc.output[i], doc.expected[i]);
                }
            }
        } else {
            assert.eq(doc.output, doc.expected);
        }
    });

    // Verify conversion from BSON array to bindata vector.
    let expectedBindataVector = doc.bindata_array_base64;

    // BSON arrays that only contain integer 0's and 1's will convert to a packed bit array.
    let canBeRepresentedAsPackedBit = array_elems.every((n) => n == 1 || n == 0);
    let intArrayCanConvertToPackedBit = canBeRepresentedAsPackedBit && dtype == kInt8Byte && array_elems.length > 0;
    if (intArrayCanConvertToPackedBit) {
        let arrayFilledWithZeros = array_elems;
        let numZeros = 0;
        while (array_elems.length % 8 != 0) {
            arrayFilledWithZeros.push("0");
            numZeros++;
        }

        expectedBindataVector = BinData(
            kBindataVectorSubtype,
            bitArrayToBase64String(
                createBindataVectorBitArray(
                    kPackedBitByte,
                    bitArrayToByteArray(arrayFilledWithZeros),
                    numZeros,
                    littleEndian,
                ),
            ),
        );
    }

    // BSON arrays that only contain integer values from [-128, 127] will convert to INT8
    // arrays.
    let canBeRepresentedAsIntArray = array_elems.every((n) => Number.isInteger(n) && n <= 127 && n >= -128);
    let floatArrayCanConvertToInt8 = canBeRepresentedAsIntArray && dtype == kFloat32Byte && array_elems.length > 0;
    if (floatArrayCanConvertToInt8) {
        expectedBindataVector = BinData(
            kBindataVectorSubtype,
            bitArrayToBase64String(createBindataVectorBitArray(kInt8Byte, array_elems, 0, littleEndian)),
        );
    }

    // Empty BSON arrays will always convert to a packed bit array.
    let arrayIsEmpty = array_elems.length == 0;
    if (arrayIsEmpty) {
        expectedBindataVector = BinData(
            kBindataVectorSubtype,
            bitArrayToBase64String(createBindataVectorBitArray(kPackedBitByte, [], 0, littleEndian)),
        );
    }

    let bsonToBindataPipeline = [
        {
            $project: {
                _id: 0,
                expected: expectedBindataVector,
                output: {
                    $convert: {
                        to: {type: "binData", subtype: 9},
                        input: "$bson_array",
                        byteOrder: littleEndian ? "little" : "big",
                    },
                },
            },
        },
    ];
    let bsonToBindataResult = coll.aggregate(bsonToBindataPipeline).toArray();
    bsonToBindataResult.forEach((doc) => {
        if (doc.approx) {
            // assert.close() does not work on arrays so manually compare each value.
            assert.eq(doc.output.length, doc.expected.length);
            for (let i = 0; i < doc.output.length; i++) {
                if (doc.output[i] == Number.NEGATIVE_INFINITY || doc.output[i] == Number.POSITIVE_INFINITY) {
                    assert.eq(doc.output[i], doc.expected[i]);
                } else {
                    assert.close(doc.output[i], doc.expected[i]);
                }
            }
        } else {
            assert.eq(doc.output, doc.expected);
        }
    });
});

/**
 * Test cases that should error when converting from bindata vector to BSON array.
 */
let binToBsonErrorCases = [
    // Invalid dtype
    {invalid_bindata_vector: "ea0000", error_code: 10506600},
    // Invalid padding - should only exist for PACKED_BIT
    {
        invalid_bindata_vector: kInt8Byte + "01" + "01",
        error_code: 10506606,
    },
    // Invalid padding - should only exist for PACKED_BIT
    {
        invalid_bindata_vector: kFloat32Byte + "01" + "11111111",
        error_code: 10506606,
    },
    // Not enough bytes for float
    {
        invalid_bindata_vector: kFloat32Byte + "00" + "001100",
        error_code: 10506602,
    },
];

binToBsonErrorCases.forEach((testCase) => {
    let bindataVectorAsBitArray = hexToBitArray(testCase.invalid_bindata_vector);
    let base64BindataArray = bitArrayToBase64String(bindataVectorAsBitArray);

    let doc = {_id: 0, bindata_array_base64: BinData(kBindataVectorSubtype, base64BindataArray)};
    const coll = db.expression_convert_bindata_vector;
    coll.drop();
    assert.commandWorked(coll.insertMany([doc]));

    // Verify conversion from bindata vector to BSON array.
    let bindataToBsonPipeline = [
        {
            $project: {_id: 0, output: {$convert: {to: {type: "array"}, input: "$bindata_array_base64"}}},
        },
    ];

    assert.throwsWithCode(() => coll.aggregate(bindataToBsonPipeline), testCase.error_code);
});

/**
 * Test cases that should error when converting from BSON array to bindata vector.
 */
let bsonToBinErrorCases = [
    // Invalid string BSON array
    {invalid_bson_array: ["oh", "hi", "mark"], error_code: ErrorCodes.ConversionFailure},
    // Must be an array
    {invalid_bson_array: "theroom", error_code: ErrorCodes.ConversionFailure},
    {invalid_bson_array: {mongodb: "skunkworks"}, error_code: ErrorCodes.ConversionFailure},
    // TODO SERVER-106059 Remove this test.
    {
        invalid_bson_array: [NumberInt(5), NumberInt(6), NumberInt(200)],
        error_code: ErrorCodes.ConversionFailure,
    },
    // TODO SERVER-106059 Remove this test.
    {
        invalid_bson_array: [NumberInt(5), NumberInt(6), NumberInt(-200)],
        error_code: ErrorCodes.ConversionFailure,
    },
];

bsonToBinErrorCases.forEach((testCase) => {
    let doc = {_id: 0, bson_array: testCase.invalid_bson_array};
    const coll = db.expression_convert_bindata_vector;
    coll.drop();
    assert.commandWorked(coll.insertMany([doc]));

    let bsonToBindataPipeline = [
        {
            $project: {
                _id: 0,
                output: {
                    $convert: {to: {type: "binData", subtype: 9}, input: "$bson_array", format: "base64"},
                },
            },
        },
    ];

    assert.throwsWithCode(() => coll.aggregate(bsonToBindataPipeline).toArray(), testCase.error_code);
});

(function bsonArrayWithLargePositiveIntFailsToBeConverted() {
    let doc = {_id: 0, bson_array: [NumberInt(5), NumberInt(6), NumberInt(200)]};
    const coll = db.expression_convert_bindata_vector;
    coll.drop();
    assert.commandWorked(coll.insertMany([doc]));

    let bsonToBindataPipeline = [
        {
            $project: {
                _id: 0,
                output: {$convert: {to: {type: "binData", subtype: 9}, input: "$bson_array"}},
            },
        },
    ];

    assert.throwsWithCode(() => coll.aggregate(bsonToBindataPipeline).toArray(), ErrorCodes.ConversionFailure);
})();
