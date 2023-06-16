/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/bson/util/simple8b_type_util.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include <absl/numeric/int128.h>
#include <boost/optional/optional.hpp>

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

using namespace mongo;

uint8_t scaleIndexForMultiplier(double multiplier) {
    auto iterIdx = std::find(Simple8bTypeUtil::kScaleMultiplier.begin(),
                             Simple8bTypeUtil::kScaleMultiplier.end(),
                             multiplier);
    if (iterIdx != Simple8bTypeUtil::kScaleMultiplier.end()) {
        return iterIdx - Simple8bTypeUtil::kScaleMultiplier.begin();
    }
    // We should never reach this
    ASSERT(false);
    return 0;
}

void assertDecimal128Equal(Decimal128 val) {
    int128_t encodeResult = Simple8bTypeUtil::encodeDecimal128(val);
    ASSERT_EQUALS(absl::Int128High64(encodeResult), val.getValue().high64);
    ASSERT_EQUALS(absl::Int128Low64(encodeResult), val.getValue().low64);
    Decimal128 decodeResult = Simple8bTypeUtil::decodeDecimal128(encodeResult);
    ASSERT_TRUE(decodeResult == val);
}

void assertBinaryEqual(char* val, size_t size, int128_t expected) {
    boost::optional<int128_t> encodeResult = Simple8bTypeUtil::encodeBinary(val, size);
    ASSERT_EQUALS(*encodeResult, expected);

    // Initialize to something non zero so we can verify that we did not write out of bounds
    char charPtr[17];
    char unused = 'x';
    memset(charPtr, unused, sizeof(charPtr));
    Simple8bTypeUtil::decodeBinary(*encodeResult, charPtr, size);
    ASSERT_EQUALS(std::memcmp(charPtr, val, size), 0);
    ASSERT_EQUALS(charPtr[size], unused);
}

void assertStringEqual(StringData val, int128_t expected) {
    boost::optional<int128_t> encodeResult = Simple8bTypeUtil::encodeString(val);
    ASSERT_EQUALS(*encodeResult, expected);

    Simple8bTypeUtil::SmallString decodeResult = Simple8bTypeUtil::decodeString(*encodeResult);
    ASSERT_EQUALS(val.size(), decodeResult.size);
    ASSERT_EQUALS(std::memcmp(val.rawData(), decodeResult.str.data(), val.size()), 0);
}

TEST(Simple8bTypeUtil, EncodeAndDecodePositiveSignedInt) {
    int64_t signedVal = 1;
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 2);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeNegativeSignedInt) {
    int64_t signedVal = -1;
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x1);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeMaxPositiveSignedInt) {
    int64_t signedVal = std::numeric_limits<int64_t>::max();
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0xFFFFFFFFFFFFFFFE);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeMaxNegativeSignedInt) {
    int64_t signedVal = std::numeric_limits<int64_t>::lowest();
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0xFFFFFFFFFFFFFFFF);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeZero) {
    int64_t signedVal = 0;
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x0);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodePositiveInt) {
    int64_t signedVal = 1234;
    // Represented as 1234 = 0...010011010010
    //  Left Shifted 0..0100110100100
    //  Right shifted 0..000000000000
    //  xor = 0..0100110100100 = 0x9A4
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x9A4);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeNegativeInt) {
    int64_t signedVal = -1234;
    // Represented as 1234 = 1...010011010010
    //  Left Shifted 0..0100110100100
    //  Right shifted 0..000000000001
    //  xor = 0..0100110100101 = 0x9A3
    uint64_t unsignedVal = Simple8bTypeUtil::encodeInt64(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x9A3);
    int64_t decodedSignedVal = Simple8bTypeUtil::decodeInt64(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, DecimalPositiveValue) {
    double val = 1.0;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(1));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), 1);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, EightDigitDecimalValue) {
    double val = 1.12345678;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(100000000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), 112345678);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, TwoDigitDecimalValue) {
    double val = 1.12;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(100));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), 112);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, FloatExceedDigitsValue) {
    double val = 1.123456789;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_FALSE(scalar);
}

TEST(Simple8bTypeUtil, SparseDecimalValue) {
    double val = 1.00000001;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(100000000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), 100000001);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, RoundingDecimalValue) {
    double val = 1.455454;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(100000000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), 145545400);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, AllNines) {
    double val = 1.99999999;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(100000000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), 199999999);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, 3DigitValue) {
    double val = 123.123;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(10000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    ASSERT_EQUALS(encodeResult.value(), (1231230));
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, RoundingTooShortShouldFail) {
    double val = 1.9876543;
    boost::optional<int64_t> encodeResult =
        Simple8bTypeUtil::encodeDouble(val, scaleIndexForMultiplier(10000));
    ASSERT_FALSE(encodeResult);
}

TEST(Simple8bTypeUtil, TestNaNAndInfinity) {
    double val = std::numeric_limits<double>::quiet_NaN();
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_FALSE(scalar);
    boost::optional<int64_t> result =
        Simple8bTypeUtil::encodeDouble(val, scaleIndexForMultiplier(100000000));
    ASSERT_FALSE(result.has_value());
    val = std::numeric_limits<double>::infinity();
    scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_FALSE(scalar);
    result = Simple8bTypeUtil::encodeDouble(val, scaleIndexForMultiplier(100000000));
    ASSERT_FALSE(result.has_value());
}

TEST(Simple8bTypeUtil, TestMaxDoubleShouldFail) {
    double val = std::numeric_limits<double>::max();
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_FALSE(scalar);
    boost::optional<int64_t> result =
        Simple8bTypeUtil::encodeDouble(val, scaleIndexForMultiplier(100000000));
    ASSERT_FALSE(result.has_value());
}

TEST(Simple8bTypeUtil, TestMinDoubleShouldFail) {
    double val = std::numeric_limits<double>::lowest();
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_FALSE(scalar);
    boost::optional<int64_t> result =
        Simple8bTypeUtil::encodeDouble(val, scaleIndexForMultiplier(100000000));
    ASSERT_FALSE(result.has_value());
}

TEST(Simple8bTypeUtil, InterpretAsMemory) {
    std::vector<double> vals = {0.0,
                                1.12345678,
                                std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::min(),
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::signaling_NaN(),
                                std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::denorm_min()};

    for (double val : vals) {
        boost::optional<int64_t> result =
            Simple8bTypeUtil::encodeDouble(val, Simple8bTypeUtil::kMemoryAsInteger);
        ASSERT_TRUE(result);

        int64_t valInt;
        memcpy(&valInt, &val, sizeof(valInt));
        ASSERT_EQ(*result, valInt);

        // Some of the special values above does not compare equal with themselves (signaling NaN).
        // Verify that we end up with the same memory after decoding
        double decoded =
            Simple8bTypeUtil::decodeDouble(*result, Simple8bTypeUtil::kMemoryAsInteger);
        ASSERT_EQ(memcmp(&decoded, &val, sizeof(val)), 0);
    }
}

TEST(Simple8bTypeUtil, TestMaxInt) {
    // max int that can be stored as a double without losing precision
    double val = std::pow(2, 53);
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(1));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    // Handle negative case
    ASSERT_EQUALS(encodeResult.value(), int64_t(val));
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, NegativeValue) {
    double val = -123.123;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(10000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    // Handle negative case
    ASSERT_EQUALS(encodeResult.value(), -1231230);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, NegativeSixDecimalValue) {
    double val = -123.123456;
    boost::optional<uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(100000000));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    // Handle negative case by subtracting 1
    ASSERT_EQUALS(encodeResult.value(), -12312345600);
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, TestMinInt) {
    // min int that can be stored as a double without losing precision
    double val = -std::pow(2, 53);
    boost::optional<std::uint8_t> scalar = Simple8bTypeUtil::calculateDecimalShiftMultiplier(val);
    ASSERT_TRUE(scalar);
    ASSERT_EQUALS(scalar.value(), scaleIndexForMultiplier(1));
    boost::optional<int64_t> encodeResult = Simple8bTypeUtil::encodeDouble(val, scalar.value());
    ASSERT_TRUE(encodeResult);
    // Handle negative case
    ASSERT_EQUALS(encodeResult.value(), int64_t(val));
    double decodeResult = Simple8bTypeUtil::decodeDouble(encodeResult.value(), scalar.value());
    ASSERT_EQUALS(val, decodeResult);
}

TEST(Simple8bTypeUtil, TestObjectId) {
    OID objId("112233445566778899AABBCC");
    int64_t encodedObjId = Simple8bTypeUtil::encodeObjectId(objId);

    int64_t expectedEncodedObjId = 0x1122AA33BB44CC;
    ASSERT_EQUALS(encodedObjId, expectedEncodedObjId);

    OID actualObjId = Simple8bTypeUtil::decodeObjectId(encodedObjId, objId.getInstanceUnique());
    ASSERT_EQUALS(objId, actualObjId);
}

TEST(Simple8bTypeUtil, EncodeAndDecodePositiveSignedInt128) {
    int128_t signedVal = 1;
    uint128_t unsignedVal = Simple8bTypeUtil::encodeInt128(signedVal);
    ASSERT_EQUALS(unsignedVal, 2);
    int128_t decodedSignedVal = Simple8bTypeUtil::decodeInt128(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeNegativeSignedInt128) {
    int128_t signedVal = -1;
    uint128_t unsignedVal = Simple8bTypeUtil::encodeInt128(signedVal);
    ASSERT_EQUALS(unsignedVal, 0x1);
    int128_t decodedSignedVal = Simple8bTypeUtil::decodeInt128(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeMaxPositiveSignedInt128) {
    int128_t signedVal = std::numeric_limits<int128_t>::max();
    uint128_t unsignedVal = Simple8bTypeUtil::encodeInt128(signedVal);
    uint128_t expectedVal = absl::MakeInt128(0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE);
    ASSERT_EQUALS(unsignedVal, expectedVal);
    int128_t decodedSignedVal = Simple8bTypeUtil::decodeInt128(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, EncodeAndDecodeMaxNegativeSignedInt128) {
    int128_t signedVal = std::numeric_limits<int128_t>::min();
    uint128_t unsignedVal = Simple8bTypeUtil::encodeInt128(signedVal);
    uint128_t expectedVal = absl::MakeInt128(0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF);
    ASSERT_EQUALS(unsignedVal, expectedVal);
    int128_t decodedSignedVal = Simple8bTypeUtil::decodeInt128(unsignedVal);
    ASSERT_EQUALS(decodedSignedVal, signedVal);
}

TEST(Simple8bTypeUtil, Decimal128Base) {
    assertDecimal128Equal(Decimal128(1.0));
}

TEST(Simple8bTypeUtil, Decimal128Negative) {
    assertDecimal128Equal(Decimal128(-1.0));
}

TEST(Simple8bTypeUtil, Decimal128Max) {
    assertDecimal128Equal(Decimal128(std::numeric_limits<Decimal128>::max()));
}

TEST(Simple8bTypeUtil, Decimal128Min) {
    assertDecimal128Equal(Decimal128(std::numeric_limits<Decimal128>::min()));
}

TEST(Simple8bTypeUtil, Decimal128Lowest) {
    assertDecimal128Equal(Decimal128(std::numeric_limits<Decimal128>::lowest()));
}

TEST(Simple8bTypeUtil, EmptyBinary) {
    // Array contents are ignored because we are passing zero for 'size'.
    char arr[1] = {'a'};
    assertBinaryEqual(arr, 0, 0);
}

TEST(Simple8bTypeUtil, SingleLetterBinary) {
    char arr[1] = {'a'};
    assertBinaryEqual(arr, sizeof(arr), 97);
}

TEST(Simple8bTypeUtil, MultiLetterBinary) {
    // a = 97 = 01100001
    // b = 98 = 01100010
    // c = 99 = 01100011
    // abc = 011000110110001001100001 = 6513249
    char arr[3] = {'a', 'b', 'c'};
    assertBinaryEqual(arr, sizeof(arr), 6513249);
}

TEST(Simple8bTypeUtil, MultiCharWithOddValues) {
    char arr[5] = {'a', char(1), '\n'};
    // a = 97 = 01100001
    // 1 = 00000001
    // \n = 00001010
    // a1\n = 000010100000000101100001 = 655713
    assertBinaryEqual(arr, sizeof(arr), 655713);
}

TEST(Simple8bTypeUtil, LargeChar) {
    char arr[15] = "abcdefghijklmn";
    assertBinaryEqual(arr, sizeof(arr), absl::MakeInt128(0x6E6D6C6B6A69, 0x6867666564636261));
}

TEST(Simple8bTypeUtil, OversizedBinary) {
    char arr[17] = "aaaaabbbbbcccccd";
    auto encoded = Simple8bTypeUtil::encodeBinary(arr, sizeof(arr));
    ASSERT_FALSE(encoded);
}

TEST(Simple8bTypeUtil, LeadingAndTrailingZeros) {
    char arr[7] = {'0', '0', '0', 'a', '0', '0', '0'};
    // 0 = 48 = 0011000
    // Our reuslt should be
    // 00110000 0011000 00110000 1100001 00110000 00110000 00110000
    assertBinaryEqual(arr, sizeof(arr), absl::MakeInt128(0, 0x30303061303030));
}

TEST(Simple8bTypeUtil, BaseString) {
    assertStringEqual("a"_sd, 97);
}

TEST(Simple8bTypeUtil, BaseString2Letter) {
    // a = 97 = 01100001
    // b = 98 = 01100010
    // reversed in little endian = 0110000101100010
    assertStringEqual("ab"_sd, 24930);
}

TEST(Simple8bTypeUtil, LargeString) {
    // a = 97 = 01100001
    // b = 98
    // c = 99
    // d = 100
    // reversed in little endian = 1100001 01100001 01100001 01100001 01100010 01100010 01100010
    // 01100010 01100011 01100011 01100011 01100011 01100100 01100100 01100100 01100100
    assertStringEqual("aaaabbbbccccdddd"_sd,
                      absl::MakeInt128(0x6161616162626262, 0x6363636364646464));
}

TEST(Simple8bTypeUtil, EmptyString) {
    assertStringEqual("", 0);
}

TEST(Simple8bTypeUtil, OddCharString) {
    // a = 97 = 01100001
    // b = 98
    // \n = 10 = 1010
    // reversed in little endian = 1100001 00001010 01100010
    assertStringEqual("a\nb", 6359650);
}

TEST(Simple8bTypeUtil, OversizdString) {
    boost::optional<int128_t> encodeResult = Simple8bTypeUtil::encodeString("aaaaabbbbbcccccdd"_sd);
    ASSERT_FALSE(encodeResult);
}

TEST(Simple8bTypeUtil, BrokenString) {
    StringData val("\0a", 3);
    boost::optional<int128_t> encodeResult = Simple8bTypeUtil::encodeString(val);
    ASSERT_FALSE(encodeResult);
}
