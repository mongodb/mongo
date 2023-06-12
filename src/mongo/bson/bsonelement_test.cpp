/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <fmt/format.h>
// IWYU pragma: no_include "ext/type_traits.h"
#include <array>
#include <cmath>
#include <limits>
#include <ostream>

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

TEST(BSONElement, BinDataToString) {
    BSONObjBuilder builder;
    unsigned char bintype0[] = {0xDE, 0xEA, 0xBE, 0xEF, 0x01};  // Random BinData shorter than UUID

    const UUID validUUID = UUID::gen();
    unsigned char zeroUUID[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned char overlongUUID[] = {0xBF,
                                    0xF7,
                                    0x1F,
                                    0x75,
                                    0x04,
                                    0x67,
                                    0x45,
                                    0xA4,
                                    0x9A,
                                    0x06,
                                    0xE9,
                                    0xBB,
                                    0x02,
                                    0x72,
                                    0x81,
                                    0x64,
                                    0xff};  // Valid RFC4122v4 UUID, but with extra byte added.
    unsigned char zeroLength[1] = {0};      // Not truly zero because Windows doesn't support that.
    StringData unknownType = "binary data\000with an unknown type"_sd;  // No terminating zero
    const BinDataType unknownBinDataType = BinDataType(42);
    builder.appendBinData("bintype0", sizeof(bintype0), BinDataGeneral, bintype0);
    validUUID.appendToBuilder(&builder, "validUUID");
    builder.appendBinData("zeroUUID", sizeof(zeroUUID), newUUID, zeroUUID);
    builder.appendBinData("overlongUUID", sizeof(overlongUUID), newUUID, overlongUUID);
    builder.appendBinData("zeroLength", 0, BinDataGeneral, zeroLength);
    builder.appendBinData(
        "unknownType", unknownType.size(), unknownBinDataType, unknownType.rawData());

    BSONObj obj = builder.obj();
    ASSERT_EQ(obj["bintype0"].toString(), "bintype0: BinData(0, DEEABEEF01)");
    ASSERT_EQ(obj["validUUID"].toString(), "validUUID: UUID(\"" + validUUID.toString() + "\")");
    ASSERT_EQ(obj["zeroUUID"].toString(),
              "zeroUUID: UUID(\"00000000-0000-0000-0000-000000000000\")");
    ASSERT_EQ(obj["overlongUUID"].toString(),
              "overlongUUID: BinData(4, BFF71F75046745A49A06E9BB02728164FF)");
    ASSERT_EQ(obj["zeroLength"].toString(), "zeroLength: BinData(0, )");
    ASSERT_EQ(obj["unknownType"].toString(),
              "unknownType: BinData(42, "
              "62696E6172792064617461007769746820616E20756E6B6E6F776E2074797065)");
}

std::string vecStr(std::vector<uint8_t> v) {
    std::string r = "[";
    StringData sep;
    for (const uint8_t& b : v) {
        r += "{}{:02x}"_format(sep, (unsigned)b);
        sep = ","_sd;
    }
    r += "]";
    return r;
}

TEST(BSONElement, BinDataVectorWithByteArrayDeprecated) {
    // ByteArrayDeprecated has a nonsense 4-byte redundant length field
    // that _binDataVector should ignore.
    std::vector<uint8_t> payload{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    std::vector<uint8_t> input = payload;

    // Insert a 4-byte prefix for the ignored ByteArrayDeprecated "length"
    std::array<uint8_t, 4> ignoredPrefix{0xcc, 0xdd, 0xee, 0xff};
    input.insert(input.begin(), ignoredPrefix.begin(), ignoredPrefix.end());

    ASSERT_EQ(vecStr(BSONObjBuilder{}
                         .appendBinData("f", input.size(), ByteArrayDeprecated, input.data())
                         .obj()["f"]
                         ._binDataVector()),
              vecStr(payload));
}

TEST(BSONElement, BinDataVectorWithBinDataGeneral) {
    std::vector<uint8_t> payload{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    std::vector<uint8_t> input = payload;
    ASSERT_EQ(vecStr(BSONObjBuilder{}
                         .appendBinData("f", input.size(), BinDataGeneral, input.data())
                         .obj()["f"]
                         ._binDataVector()),
              vecStr(payload));
}


TEST(BSONElement, TimestampToString) {

    // Testing default BSONObj Timestamp method, which constructs an empty Timestamp
    const BSONElement b;
    auto ts = b.timestamp();
    ASSERT_EQ(ts.toString(), "Timestamp(0, 0)");

    BSONObjBuilder builder;
    builder.append("ts0", Timestamp(Seconds(100), 1U));
    builder.append("ts1", Timestamp(Seconds(50000), 25));
    builder.append("ts2", Timestamp(Seconds(100000), 1U));
    // Testing max allowable integer values
    builder.append("ts3", Timestamp::max());

    // Testing for correct format when printing BSONObj Timestamps
    // using .toString(includeFieldName = false, full = false)
    BSONObj obj = builder.obj();
    ASSERT_EQ(obj["ts0"].toString(false, false), "Timestamp(100, 1)");
    ASSERT_EQ(obj["ts1"].toString(false, false), "Timestamp(50000, 25)");
    ASSERT_EQ(obj["ts2"].toString(false, false), "Timestamp(100000, 1)");
    ASSERT_EQ(obj["ts3"].toString(false, false), "Timestamp(4294967295, 4294967295)");
}

TEST(BSONElement, ExtractLargeSubObject) {
    std::int32_t size = 17 * 1024 * 1024;
    std::vector<char> buffer(size);
    DataRange bufferRange(&buffer.front(), &buffer.back());
    ASSERT_OK(bufferRange.writeNoThrow(LittleEndian<int32_t>(size)));

    BSONObj obj(buffer.data(), BSONObj::LargeSizeTrait{});

    BSONObjBuilder bigObjectBuilder;
    bigObjectBuilder.append("a", obj);
    BSONObj bigObj = bigObjectBuilder.obj<BSONObj::LargeSizeTrait>();

    BSONElement element = bigObj["a"];
    ASSERT_EQ(BSONType::Object, element.type());

    BSONObj subObj = element.Obj();
}

TEST(BSONElement, SafeNumberLongPositiveBound) {
    BSONObj obj =
        BSON("kLongLongMaxPlusOneAsDouble"
             << BSONElement::kLongLongMaxPlusOneAsDouble << "towardsZero"
             << std::nextafter(BSONElement::kLongLongMaxPlusOneAsDouble, 0.0) << "towardsInfinity"
             << std::nextafter(BSONElement::kLongLongMaxPlusOneAsDouble,
                               std::numeric_limits<double>::max())
             << "positiveInfinity" << std::numeric_limits<double>::infinity());

    // kLongLongMaxPlusOneAsDouble is the least double value that will overflow a 64-bit signed
    // two's-complement integer. Historically, converting this value with safeNumberLong() would
    // return the result of casting to double with a C-style cast. That operation is undefined
    // because of the overflow, but on most platforms we support, it returned the min 64-bit value
    // (-2^63). The safeNumberLongForHash() function should preserve that behavior indefinitely for
    // compatibility with on-disk data.
    ASSERT_EQ(obj["kLongLongMaxPlusOneAsDouble"].safeNumberLongForHash(),
              std::numeric_limits<long long>::lowest());

    // The safeNumberLong() function clamps kLongLongMaxPlusOneAsDouble to the max 64-bit value
    // (2^63 - 1).
    ASSERT_EQ(obj["kLongLongMaxPlusOneAsDouble"].safeNumberLong(),
              std::numeric_limits<long long>::max());

    // One quantum below kLongLongMaxPlusOneAsDouble is the largest double that safely converts to a
    // 64-bit signed two-s complement integer. Both safeNumberLong() and safeNumberLongForHash()
    // convert this using a C or C-style cast, an operation with defined behavior. This conversion
    // is exact.
    ASSERT_EQ(obj["towardsZero"].safeNumberLongForHash(), 0x7ffffffffffffc00ll);
    ASSERT_EQ(obj["towardsZero"].safeNumberLong(), 0x7ffffffffffffc00ll);

    // One quantum above kLongLongMaxPlusOneAsDouble is another number that that is too large to
    // convert. The safeNumberLong() function has always clamped this value to the max 64-bit value
    // (2^63 - 1), and that should continue to be the behavior for both safeNumberLong() and
    // safeNumberLongForHash().
    ASSERT_EQ(obj["towardsInfinity"].safeNumberLongForHash(),
              std::numeric_limits<long long>::max());
    ASSERT_EQ(obj["towardsInfinity"].safeNumberLong(), std::numeric_limits<long long>::max());

    // Both safeNumberLong() and safeNumberLongForHash() also clamp positive infinity to the max
    // 64-bit value (2^63 - 1).
    ASSERT_EQ(obj["positiveInfinity"].safeNumberLongForHash(),
              std::numeric_limits<long long>::max());
    ASSERT_EQ(obj["positiveInfinity"].safeNumberLong(), std::numeric_limits<long long>::max());
}

TEST(BSONElement, SafeNumberLongNegativeBound) {
    // Unlike the max long long value, the least long long value (-2^63) converts exactly to a
    // double value and can safely be used as a bound to check which double values are in the range
    // of long long.
    const double lowestLongLongAsDouble =
        static_cast<double>(std::numeric_limits<long long>::lowest());
    BSONObj obj =
        BSON("lowestLongLongAsDouble"  // This comment forces clang-format to break here.
             << lowestLongLongAsDouble << "towardsZero"
             << std::nextafter(lowestLongLongAsDouble, 0.0) << "towardsNegativeInfinity"
             << std::nextafter(lowestLongLongAsDouble, std::numeric_limits<double>::lowest())
             << "negativeInfinity" << -std::numeric_limits<double>::infinity());

    ASSERT_EQ(obj["lowestLongLongAsDouble"].safeNumberLongForHash(),
              std::numeric_limits<long long>::lowest());
    ASSERT_EQ(obj["lowestLongLongAsDouble"].safeNumberLong(),
              std::numeric_limits<long long>::lowest());

    ASSERT_EQ(obj["towardsZero"].safeNumberLongForHash(), -0x7ffffffffffffc00);
    ASSERT_EQ(obj["towardsZero"].safeNumberLong(), -0x7ffffffffffffc00);

    ASSERT_EQ(obj["towardsNegativeInfinity"].safeNumberLongForHash(),
              std::numeric_limits<long long>::lowest());
    ASSERT_EQ(obj["towardsNegativeInfinity"].safeNumberLong(),
              std::numeric_limits<long long>::lowest());

    ASSERT_EQ(obj["negativeInfinity"].safeNumberLongForHash(),
              std::numeric_limits<long long>::lowest());
    ASSERT_EQ(obj["negativeInfinity"].safeNumberLong(), std::numeric_limits<long long>::lowest());
}

TEST(BSONElement, SafeNumberDoublePositiveBound) {
    BSONObj obj = BSON("kLargestSafeLongLongAsDouble"
                       << BSONElement::kLargestSafeLongLongAsDouble << "towardsZero"
                       << BSONElement::kLargestSafeLongLongAsDouble - 1 << "towardsInfinity"
                       << BSONElement::kLargestSafeLongLongAsDouble + 1 << "positiveInfinity"
                       << std::numeric_limits<long long>::max());

    ASSERT_EQ(obj["kLargestSafeLongLongAsDouble"].safeNumberDouble(),
              (double)BSONElement::kLargestSafeLongLongAsDouble);
    ASSERT_EQ(obj["towardsZero"].safeNumberDouble(),
              (double)(BSONElement::kLargestSafeLongLongAsDouble - 1));
    ASSERT_EQ(obj["towardsInfinity"].safeNumberDouble(),
              (double)BSONElement::kLargestSafeLongLongAsDouble);
    ASSERT_EQ(obj["positiveInfinity"].safeNumberDouble(),
              (double)BSONElement::kLargestSafeLongLongAsDouble);
}

TEST(BSONElement, SafeNumberDoubleNegativeBound) {
    BSONObj obj =
        BSON("kSmallestSafeLongLongAsDouble"
             << BSONElement::kSmallestSafeLongLongAsDouble << "towardsZero"
             << BSONElement::kSmallestSafeLongLongAsDouble + 1 << "towardsNegativeInfinity"
             << BSONElement::kSmallestSafeLongLongAsDouble - 1 << "negativeInfinity"
             << std::numeric_limits<long long>::min());

    ASSERT_EQ(obj["kSmallestSafeLongLongAsDouble"].safeNumberDouble(),
              (double)BSONElement::kSmallestSafeLongLongAsDouble);
    ASSERT_EQ(obj["towardsZero"].safeNumberDouble(),
              (double)(BSONElement::kSmallestSafeLongLongAsDouble + 1));
    ASSERT_EQ(obj["towardsNegativeInfinity"].safeNumberDouble(),
              (double)BSONElement::kSmallestSafeLongLongAsDouble);
    ASSERT_EQ(obj["negativeInfinity"].safeNumberDouble(),
              (double)BSONElement::kSmallestSafeLongLongAsDouble);
}

TEST(BSONElement, IsNaN) {
    ASSERT(BSON("" << std::numeric_limits<double>::quiet_NaN()).firstElement().isNaN());
    ASSERT(BSON("" << -std::numeric_limits<double>::quiet_NaN()).firstElement().isNaN());
    ASSERT(BSON("" << Decimal128::kPositiveNaN).firstElement().isNaN());
    ASSERT(BSON("" << Decimal128::kNegativeNaN).firstElement().isNaN());

    ASSERT_FALSE(BSON("" << std::numeric_limits<double>::infinity()).firstElement().isNaN());
    ASSERT_FALSE(BSON("" << -std::numeric_limits<double>::infinity()).firstElement().isNaN());
    ASSERT_FALSE(BSON("" << Decimal128::kPositiveInfinity).firstElement().isNaN());
    ASSERT_FALSE(BSON("" << Decimal128::kNegativeInfinity).firstElement().isNaN());
    ASSERT_FALSE(BSON("" << Decimal128{"9223372036854775808.5"}).firstElement().isNaN());
    ASSERT_FALSE(BSON("" << Decimal128{"-9223372036854775809.99"}).firstElement().isNaN());
    ASSERT_FALSE(BSON("" << 12345LL).firstElement().isNaN());
    ASSERT_FALSE(BSON(""
                      << "foo")
                     .firstElement()
                     .isNaN());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongRejectsNegative) {
    BSONObj query = BSON("" << -2LL);
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToNonNegativeLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongAcceptsNegative) {
    BSONObj query = BSON("" << -2LL);
    auto result = query.firstElement().parseIntegerElementToLong();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(-2LL, result.getValue());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongRejectsTooLargeDouble) {
    BSONObj query = BSON("" << BSONElement::kLongLongMaxPlusOneAsDouble);
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToNonNegativeLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongRejectsTooLargeDouble) {
    BSONObj query = BSON("" << BSONElement::kLongLongMaxPlusOneAsDouble);
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongRejectsTooLargeNegativeDouble) {
    BSONObj query = BSON("" << std::numeric_limits<double>::min());
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongRejectsString) {
    BSONObj query = BSON(""
                         << "1");
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToNonNegativeLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongRejectsString) {
    BSONObj query = BSON(""
                         << "1");
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongRejectsNonIntegralDouble) {
    BSONObj query = BSON("" << 2.5);
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToNonNegativeLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongRejectsNonIntegralDouble) {
    BSONObj query = BSON("" << 2.5);
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongRejectsNonIntegralDecimal) {
    BSONObj query = BSON("" << Decimal128("2.5"));
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToNonNegativeLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongRejectsNonIntegralDecimal) {
    BSONObj query = BSON("" << Decimal128("2.5"));
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongRejectsLargestDecimal) {
    BSONObj query = BSON("" << Decimal128(Decimal128::kLargestPositive));
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToNonNegativeLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongRejectsLargestDecimal) {
    BSONObj query = BSON("" << Decimal128(Decimal128::kLargestPositive));
    ASSERT_NOT_OK(query.firstElement().parseIntegerElementToLong());
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongAcceptsZero) {
    BSONObj query = BSON("" << 0);
    auto result = query.firstElement().parseIntegerElementToNonNegativeLong();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 0LL);
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongAcceptsZero) {
    BSONObj query = BSON("" << 0);
    auto result = query.firstElement().parseIntegerElementToLong();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 0LL);
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToNonNegativeLongAcceptsThree) {
    BSONObj query = BSON("" << 3.0);
    auto result = query.firstElement().parseIntegerElementToNonNegativeLong();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 3LL);
}

TEST(BSONElementIntegerParseTest, ParseIntegerElementToLongAcceptsThree) {
    BSONObj query = BSON("" << 3.0);
    auto result = query.firstElement().parseIntegerElementToLong();
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 3LL);
}

TEST(BSONElementTryCoeceToLongLongTest, CoerceSucceeds) {
    struct TestCase {
        BSONObj inputDocument;
        long long expectedResult;
    };
    std::vector<TestCase> testCases{
        {BSON("" << 3.0), 3},
        {BSON("" << 4.5), 4},
        {BSON("" << 5.5000001), 5},
        {BSON("" << -5.5), -5},
        {BSON("" << -5.500001), -5},
        {BSON("" << (-std::exp2(63))), std::numeric_limits<long long>::min()},
        {BSON("" << 36028797018963968.0), 36028797018963968LL},  // Representable integer in double.
        {BSON("" << Decimal128{"5.6"}), 5},
        {BSON("" << Decimal128{"5.5"}), 5},
        {BSON("" << Decimal128{"-1.5"}), -1},
        {BSON("" << Decimal128{"-1.500001"}), -1},
        {BSON("" << Decimal128{"9223372036854775807.5"}), std::numeric_limits<long long>::max()},
        {BSON("" << Decimal128{"-9223372036854775808.99"}), std::numeric_limits<long long>::min()}};
    for (auto&& testCase : testCases) {
        long long number;
        auto result = testCase.inputDocument.firstElement().tryCoerce(&number);
        ASSERT_OK(result);
        ASSERT_EQUALS(number, testCase.expectedResult)
            << " for input document " << testCase.inputDocument.toString();
    }
}

TEST(BSONElementTryCoeceToLongLongTest, CoerceFails) {
    std::vector<BSONObj> testCases{BSON("" << std::numeric_limits<double>::quiet_NaN()),
                                   BSON("" << std::numeric_limits<double>::infinity()),
                                   BSON("" << -std::numeric_limits<double>::quiet_NaN()),
                                   BSON("" << -std::numeric_limits<double>::infinity()),
                                   BSON("" << (std::exp2(63) + (1 << 11))),
                                   BSON("" << -std::exp2(63) - (1 << 11)),
                                   BSON("" << Decimal128::kPositiveNaN),
                                   BSON("" << Decimal128::kPositiveInfinity),
                                   BSON("" << Decimal128::kNegativeNaN),
                                   BSON("" << Decimal128::kNegativeInfinity),
                                   BSON("" << Decimal128{"9223372036854775808.5"}),
                                   BSON("" << Decimal128{"-9223372036854775809.99"})};
    for (auto&& testCase : testCases) {
        long long number;
        auto result = testCase.firstElement().tryCoerce(&number);
        ASSERT_NOT_OK(result) << " for input document " << testCase.toString();
    }
}
}  // namespace
}  // namespace mongo
