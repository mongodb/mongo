/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/represent_as.h"

#include <cstdint>
#include <cstring>
#include <limits>


namespace mongo::sbe {

TEST(SBEValues, Basic) {
    {
        const auto [tag, val] = value::makeNewString("small"_sd);
        ASSERT_EQUALS(tag, value::TypeTags::StringSmall);

        value::releaseValue(tag, val);
    }

    {
        const auto [tag, val] = value::makeNewString("not so small string"_sd);
        ASSERT_EQUALS(tag, value::TypeTags::StringBig);

        value::releaseValue(tag, val);
    }
    {
        const auto [tag, val] = value::makeNewObject();
        auto obj = value::getObjectView(val);

        const auto [fieldTag, fieldVal] = value::makeNewString("not so small string"_sd);
        obj->push_back("field"_sd, fieldTag, fieldVal);

        ASSERT_EQUALS(obj->size(), 1);
        const auto [checkTag, checkVal] = obj->getField("field"_sd);

        ASSERT_EQUALS(fieldTag, checkTag);
        ASSERT_EQUALS(fieldVal, checkVal);

        value::releaseValue(tag, val);
    }
    {
        const auto [tag, val] = value::makeNewArray();
        auto obj = value::getArrayView(val);

        const auto [fieldTag, fieldVal] = value::makeNewString("not so small string"_sd);
        obj->push_back(fieldTag, fieldVal);

        ASSERT_EQUALS(obj->size(), 1);
        const auto [checkTag, checkVal] = obj->getAt(0);

        ASSERT_EQUALS(fieldTag, checkTag);
        ASSERT_EQUALS(fieldVal, checkVal);

        value::releaseValue(tag, val);
    }
}

TEST(SBEValues, Hash) {
    auto tagInt32 = value::TypeTags::NumberInt32;
    auto valInt32 = value::bitcastFrom<int32_t>(-5);

    auto tagInt64 = value::TypeTags::NumberInt64;
    auto valInt64 = value::bitcastFrom<int64_t>(-5);

    auto tagDouble = value::TypeTags::NumberDouble;
    auto valDouble = value::bitcastFrom<double>(-5.0);

    auto [tagDecimal, valDecimal] = value::makeCopyDecimal(mongo::Decimal128(-5.0));

    ASSERT_EQUALS(value::hashValue(tagInt32, valInt32), value::hashValue(tagInt64, valInt64));
    ASSERT_EQUALS(value::hashValue(tagInt32, valInt32), value::hashValue(tagDouble, valDouble));
    ASSERT_EQUALS(value::hashValue(tagInt32, valInt32), value::hashValue(tagDecimal, valDecimal));

    value::releaseValue(tagDecimal, valDecimal);

    auto tagDoubleInf = value::TypeTags::NumberDouble;
    auto valDoubleInf = value::bitcastFrom<double>(std::numeric_limits<double>::infinity());

    auto [tagDecimalInf, valDecimalInf] =
        value::makeCopyDecimal(mongo::Decimal128(std::numeric_limits<double>::infinity()));

    ASSERT_EQUALS(value::hashValue(tagDoubleInf, valDoubleInf),
                  value::hashValue(tagDecimalInf, valDecimalInf));

    value::releaseValue(tagDecimalInf, valDecimalInf);

    auto testDoubleVsDecimal = [](double doubleValue, double decimalValue) {
        auto tagDouble = value::TypeTags::NumberDouble;
        auto valDouble = value::bitcastFrom<double>(doubleValue);

        auto [tagDecimal, valDecimal] = value::makeCopyDecimal(mongo::Decimal128(decimalValue));
        value::ValueGuard guard{tagDecimal, valDecimal};

        ASSERT_EQUALS(value::hashValue(tagDouble, valDouble),
                      value::hashValue(tagDecimal, valDecimal));
    };

    // Test bitwise identical NaNs.
    testDoubleVsDecimal(std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN());

    // Test NaNs with different bit representation.
    if constexpr (std::numeric_limits<double>::has_signaling_NaN) {
        const auto firstNan = std::numeric_limits<double>::quiet_NaN();
        const auto secondNan = std::numeric_limits<double>::signaling_NaN();
        auto getDoubleBits = [](double value) {
            uint64_t bits = 0;
            memcpy(&bits, &value, sizeof(value));
            return bits;
        };
        ASSERT_NOT_EQUALS(getDoubleBits(firstNan), getDoubleBits(secondNan));
        testDoubleVsDecimal(firstNan, secondNan);
    }

    // Start with a relatively large number that still fits in 64 bits.
    int64_t num = 0x7000000000000000;
    // And is exactly representable as double (there is enough zeroes in the right part of the
    // number so we should be ok).
    auto asDbl = representAs<double>(num);
    ASSERT(asDbl);

    // Now "shift" the number to the left by 10 bits making it out of range for 64 bit integer.
    double asBigDbl = (*asDbl) * 1024.0;
    ASSERT(!representAs<int64_t>(asBigDbl));

    // Sanity check.
    ASSERT(asBigDbl / 1024.0 == (*asDbl));

    auto asDec = Decimal128(asBigDbl, Decimal128::kRoundTo34Digits);
    ASSERT(asDec.toDouble() == asBigDbl);

    auto tagDoubleBig = value::TypeTags::NumberDouble;
    auto valDoubleBig = value::bitcastFrom<double>(asBigDbl);

    auto [tagDecimalBig, valDecimalBig] = value::makeCopyDecimal(asDec);

    ASSERT_EQUALS(value::hashValue(tagDoubleBig, valDoubleBig),
                  value::hashValue(tagDecimalBig, valDecimalBig));

    value::releaseValue(tagDecimalBig, valDecimalBig);

    uint8_t byteArray1[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t byteArray2[] = {4, 3, 2, 1, 5, 6, 7, 8};
    auto binDataOperands =
        BSON_ARRAY(BSONBinData(byteArray1, sizeof(byteArray1), BinDataGeneral)
                   << BSONBinData(byteArray1, sizeof(byteArray1), ByteArrayDeprecated)
                   << BSONBinData(byteArray2, sizeof(byteArray2), ByteArrayDeprecated));

    // Two BinData values with the same data but different subtypes should hash differently.
    auto tagGeneralBinData = value::TypeTags::bsonBinData;
    auto valGeneralBinData = value::bitcastFrom<const char*>(binDataOperands[0].value());

    auto tagDeprecatedBinData1 = value::TypeTags::bsonBinData;
    auto valDeprecatedBinData1 = value::bitcastFrom<const char*>(binDataOperands[1].value());

    ASSERT_NE(value::hashValue(tagGeneralBinData, valGeneralBinData),
              value::hashValue(tagDeprecatedBinData1, valDeprecatedBinData1));

    // Two ByteArrayDeprecated BinData values with different values in the leading four bytes should
    // hash differently, even though those four bytes are technically not part of the binary data
    // payload.
    auto tagDeprecatedBinData2 = value::TypeTags::bsonBinData;
    auto valDeprecatedBinData2 = value::bitcastFrom<const char*>(binDataOperands[2].value());

    ASSERT_NE(value::hashValue(tagDeprecatedBinData1, valDeprecatedBinData1),
              value::hashValue(tagDeprecatedBinData2, valDeprecatedBinData2));
}

TEST(SBEValues, HashCompound) {
    using namespace std::literals;
    {
        auto [tag1, val1] = value::makeNewArray();
        auto arr1 = value::getArrayView(val1);
        arr1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        arr1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        arr1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-7));

        auto [tag2, val2] = value::makeNewArray();
        auto arr2 = value::getArrayView(val2);
        arr2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));
        arr2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6.0));
        arr2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-7.0));

        ASSERT_EQUALS(value::hashValue(tag1, val1), value::hashValue(tag2, val2));

        value::releaseValue(tag1, val1);
        value::releaseValue(tag2, val2);
    }
    {
        auto [tag1, val1] = value::makeNewObject();
        auto obj1 = value::getObjectView(val1);
        obj1->push_back("a"_sd, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        obj1->push_back("b"_sd, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        obj1->push_back("c"_sd, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-7));

        auto [tag2, val2] = value::makeNewObject();
        auto obj2 = value::getObjectView(val2);
        obj2->push_back("a"_sd, value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));
        obj2->push_back("b"_sd, value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6.0));
        obj2->push_back("c"_sd, value::TypeTags::NumberDouble, value::bitcastFrom<double>(-7.0));

        ASSERT_EQUALS(value::hashValue(tag1, val1), value::hashValue(tag2, val2));

        value::releaseValue(tag1, val1);
        value::releaseValue(tag2, val2);
    }

    {
        auto [tag1, val1] = value::makeNewArraySet();
        auto set1 = value::getArraySetView(val1);
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-7));

        auto [tag2, val2] = value::makeNewArraySet();
        auto set2 = value::getArraySetView(val2);
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-7.0));
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6.0));
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));


        ASSERT_EQUALS(value::hashValue(tag1, val1), value::hashValue(tag2, val2));

        value::releaseValue(tag1, val1);
        value::releaseValue(tag2, val2);
    }

    {
        auto [tag1, val1] = value::makeNewArrayMultiSet();
        auto set1 = value::getArrayMultiSetView(val1);
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        set1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-7));

        auto [tag2, val2] = value::makeNewArrayMultiSet();
        auto set2 = value::getArrayMultiSetView(val2);
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-7.0));
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6.0));
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));
        set2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));

        ASSERT_EQUALS(value::hashValue(tag1, val1), value::hashValue(tag2, val2));

        value::releaseValue(tag1, val1);
        value::releaseValue(tag2, val2);
    }
}

TEST(SBEVM, Add) {
    {
        auto tagInt32 = value::TypeTags::NumberInt32;
        auto valInt32 = value::bitcastFrom<int32_t>(-7);

        auto tagInt64 = value::TypeTags::NumberInt64;
        auto valInt64 = value::bitcastFrom<int64_t>(-5);

        vm::CodeFragment code;
        code.appendConstVal(tagInt32, valInt32);
        code.appendConstVal(tagInt64, valInt64);
        code.appendAdd({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQUALS(tag, value::TypeTags::NumberInt64);
        ASSERT_EQUALS(value::bitcastTo<int64_t>(val), -12);
    }
    {
        auto tagInt32 = value::TypeTags::NumberInt32;
        auto valInt32 = value::bitcastFrom<int32_t>(-7);

        auto tagDouble = value::TypeTags::NumberDouble;
        auto valDouble = value::bitcastFrom<double>(-5.0);

        vm::CodeFragment code;
        code.appendConstVal(tagInt32, valInt32);
        code.appendConstVal(tagDouble, valDouble);
        code.appendAdd({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQUALS(tag, value::TypeTags::NumberDouble);
        ASSERT_EQUALS(value::bitcastTo<double>(val), -12.0);
    }
    {
        auto [tagDecimal, valDecimal] = value::makeCopyDecimal(mongo::Decimal128(-7.25));

        auto tagDouble = value::TypeTags::NumberDouble;
        auto valDouble = value::bitcastFrom<double>(-5.25);

        vm::CodeFragment code;
        code.appendConstVal(tagDecimal, valDecimal);
        code.appendConstVal(tagDouble, valDouble);
        code.appendAdd({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQUALS(tag, value::TypeTags::NumberDecimal);
        ASSERT_EQUALS(value::bitcastTo<mongo::Decimal128>(val).toDouble(), -12.5);
        ASSERT_TRUE(owned);

        value::releaseValue(tag, val);
        value::releaseValue(tagDecimal, valDecimal);
    }
}

TEST(SBEVM, CompareBinData) {
    {
        uint8_t byteArray1[] = {1, 2, 3, 4};
        uint8_t byteArray2[] = {1, 2, 3, 10};
        auto operands = BSON_ARRAY(BSONBinData(byteArray1, sizeof(byteArray1), BinDataGeneral)
                                   << BSONBinData(byteArray2, sizeof(byteArray2), BinDataGeneral));

        vm::CodeFragment code;
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[0].value()));
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[1].value()));
        code.appendCmp3w({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        ASSERT_LT(value::bitcastTo<int32_t>(val), 0);
        ASSERT_FALSE(owned);
    }
    {
        uint8_t byteArray1[] = {1, 2, 3, 4};
        uint8_t byteArray2[] = {1, 2, 3, 4};
        auto operands = BSON_ARRAY(BSONBinData(byteArray1, sizeof(byteArray1), BinDataGeneral)
                                   << BSONBinData(byteArray2, sizeof(byteArray2), BinDataGeneral));

        vm::CodeFragment code;
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[0].value()));
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[1].value()));
        code.appendCmp3w({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(val), 0);
        ASSERT_FALSE(owned);
    }
    {
        uint8_t byteArray1[] = {1, 2, 10, 4};
        uint8_t byteArray2[] = {1, 2, 3, 4};
        auto operands = BSON_ARRAY(BSONBinData(byteArray1, sizeof(byteArray1), BinDataGeneral)
                                   << BSONBinData(byteArray2, sizeof(byteArray2), BinDataGeneral));

        vm::CodeFragment code;
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[0].value()));
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[1].value()));
        code.appendCmp3w({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        ASSERT_GT(value::bitcastTo<int32_t>(val), 0);
        ASSERT_FALSE(owned);
    }

    // BinData values are ordered by subtype. Values with different subtypes should compare as not
    // equal, even if they have the same data.
    {
        uint8_t byteArray1[] = {1, 2, 3, 4};
        uint8_t byteArray2[] = {1, 2, 3, 4};
        auto operands =
            BSON_ARRAY(BSONBinData(byteArray1, sizeof(byteArray1), BinDataGeneral)
                       << BSONBinData(byteArray2, sizeof(byteArray2), ByteArrayDeprecated));

        vm::CodeFragment code;
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[0].value()));
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[1].value()));
        code.appendCmp3w({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        ASSERT_LT(value::bitcastTo<int32_t>(val), 0);
        ASSERT_FALSE(owned);
    }

    // Comparison of 'ByteArrayDeprecated' BinData values should consider the leading four bytes,
    // even those those bytes are not part of the data payload, according to the standard.
    {
        uint8_t byteArray1[] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t byteArray2[] = {11, 12, 13, 14, 5, 6, 7, 8};
        auto operands =
            BSON_ARRAY(BSONBinData(byteArray1, sizeof(byteArray1), ByteArrayDeprecated)
                       << BSONBinData(byteArray2, sizeof(byteArray2), ByteArrayDeprecated));

        vm::CodeFragment code;
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[0].value()));
        code.appendConstVal(value::TypeTags::bsonBinData,
                            value::bitcastFrom<const char*>(operands[1].value()));
        code.appendCmp3w({}, {});

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        ASSERT_LT(value::bitcastTo<int32_t>(val), 0);
        ASSERT_FALSE(owned);
    }
}

TEST(SBEVM, ConvertBinDataToBsonObj) {
    uint8_t byteArray[] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto originalBinData =
        BSON_ARRAY(BSONBinData(byteArray, sizeof(byteArray), ByteArrayDeprecated));

    value::Array array;
    auto [binDataTag, binDataVal] = value::copyValue(
        value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(originalBinData[0].value()));
    array.push_back(binDataTag, binDataVal);

    BSONArrayBuilder builder;
    bson::convertToBsonArr(builder, &array);
    auto convertedBinData = builder.done();

    ASSERT_EQ(originalBinData.woCompare(convertedBinData), 0);
}

TEST(SBEVM, CodeFragmentToStringSanity) {
    vm::CodeFragment code;
    auto ptr2str = [](const void* ptr) {
        std::stringstream ss;
        ss << ptr;
        return ss.str();
    };

    code.appendDiv({}, {});
    std::string instrs = code.toString();

    ASSERT_TRUE(instrs.find("[" + ptr2str(code.instrs().data()) + "]: div") >= 0);
}

TEST(SBEVM, CodeFragmentPrintStable) {
    GoldenTestContext ctx(&goldenTestConfigSbe);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    auto& os = ctx.outStream();

    vm::CodeFragment code;
    code.appendFillEmpty(vm::Instruction::Null);
    code.appendFillEmpty(vm::Instruction::False);
    code.appendFillEmpty(vm::Instruction::True);
    code.appendTraverseP(0xAA, 1, vm::Instruction::Nothing);
    code.appendTraverseP(0xAA, 1, vm::Instruction::Int32One);
    code.appendTraverseF(0xBB, 1, vm::Instruction::True);
    code.appendGetField({}, "Hello world!"_sd);
    code.appendAdd({}, {});

    TimeZoneDatabase timezoneDB;
    code.appendDateTrunc(
        TimeUnit::day, 1, timezoneDB.getTimeZone("America/New_York"_sd), DayOfWeek::monday);

    vm::CodeFragmentPrinter printer(vm::CodeFragmentPrinter::PrintFormat::Stable);
    printer.print(os, code);
    os << std::endl;
}

namespace {

/**
 * Fills bytes after the null terminator in the string with 'pattern'.
 *
 * We use this function in the tests to ensure that the implementation of 'getStringLength' for
 * 'StringSmall' type does not rely on the fact that all bytes after null terminator are zero or any
 * other special value.
 */
void fillSmallStringTail(value::Value val, char pattern) {
    char* rawView = value::getRawStringView(value::TypeTags::StringSmall, val);
    for (auto i = std::strlen(rawView) + 1; i <= value::kSmallStringMaxLength; i++) {
        rawView[i] = pattern;
    }
}
}  // namespace

TEST(SBESmallString, Length) {
    std::vector<std::pair<std::string, size_t>> testCases{
        {"", 0},
        {"a", 1},
        {"ab", 2},
        {"abc", 3},
        {"abcd", 4},
        {"abcde", 5},
        {"abcdef", 6},
        {"abcdefh", 7},
    };

    for (const auto& [string, length] : testCases) {
        ASSERT(value::canUseSmallString(string));
        auto [tag, val] = value::makeSmallString(string);
        ASSERT_EQ(tag, value::TypeTags::StringSmall);

        fillSmallStringTail(val, char(0));
        ASSERT_EQ(length, value::getStringLength(tag, val));

        fillSmallStringTail(val, char(1));
        ASSERT_EQ(length, value::getStringLength(tag, val));

        fillSmallStringTail(val, char(1 << 7));
        ASSERT_EQ(length, value::getStringLength(tag, val));

        fillSmallStringTail(val, ~char(0));
        ASSERT_EQ(length, value::getStringLength(tag, val));
    }
}
}  // namespace mongo::sbe
