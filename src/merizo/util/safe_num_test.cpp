/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include <limits>

#include "merizo/platform/basic.h"
#undef MONGO_PCH_WHITELISTED  // for malloc/realloc pulled from bson

#include "merizo/bson/bsonobj.h"
#include "merizo/bson/bsontypes.h"
#include "merizo/platform/decimal128.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/safe_num.h"

namespace {

using merizo::SafeNum;
using merizo::Decimal128;

TEST(Basics, Initialization) {
    const SafeNum numInt(0);
    ASSERT_EQUALS(numInt.type(), merizo::NumberInt);

    const SafeNum numLong(static_cast<int64_t>(0));
    ASSERT_EQUALS(numLong.type(), merizo::NumberLong);

    const SafeNum numDouble(0.0);
    ASSERT_EQUALS(numDouble.type(), merizo::NumberDouble);

    const SafeNum numDecimal(Decimal128("1.0"));
    ASSERT_EQUALS(numDecimal.type(), merizo::NumberDecimal);
}

TEST(Basics, BSONElementInitialization) {
    merizo::BSONObj o;
    o = BSON("numberInt" << 1 << "numberLong" << 1LL << "numberDouble" << 0.1 << "NumberDecimal"
                         << Decimal128("1"));

    const SafeNum numInt(o.getField("numberInt"));
    ASSERT_EQUALS(numInt.type(), merizo::NumberInt);

    const SafeNum numLong(o.getField("numberLong"));
    ASSERT_EQUALS(numLong.type(), merizo::NumberLong);

    const SafeNum numDouble(o.getField("numberDouble"));
    ASSERT_EQUALS(numDouble.type(), merizo::NumberDouble);

    const SafeNum numDecimal(o.getField("NumberDecimal"));
    ASSERT_EQUALS(numDecimal.type(), merizo::NumberDecimal);
}

TEST(Comparison, EOO) {
    const SafeNum safeNumA;
    const SafeNum safeNumB;
    ASSERT_FALSE(safeNumA.isValid());
    ASSERT_FALSE(safeNumB.isValid());
    ASSERT_EQUALS(safeNumA.type(), merizo::EOO);
    ASSERT_EQUALS(safeNumB.type(), merizo::EOO);
    ASSERT_TRUE(safeNumA.isEquivalent(safeNumB));
    ASSERT_FALSE(safeNumA.isIdentical(safeNumB));

    const SafeNum one(1);
    ASSERT_NOT_EQUALS(one, safeNumA);
}

TEST(Comparison, StrictTypeComparison) {
    const SafeNum one(1);
    const SafeNum oneLong((static_cast<int64_t>(1)));
    const SafeNum oneDouble(1.0);
    ASSERT_FALSE(one.isIdentical(oneLong));
    ASSERT_FALSE(oneLong.isIdentical(oneDouble));
    ASSERT_FALSE(oneDouble.isIdentical(one));
    ASSERT_TRUE(oneDouble.isIdentical(oneDouble));

    const SafeNum oneDecimal(Decimal128(1));
    ASSERT_FALSE(oneDecimal.isIdentical(one));
    ASSERT_TRUE(oneDecimal.isIdentical(oneDecimal));
}

TEST(Comparison, EquivalenceComparisonNormal) {
    const SafeNum one(1);
    const SafeNum oneLong(static_cast<int64_t>(1));
    const SafeNum oneDouble(1.0);
    ASSERT_EQUALS(one, oneLong);
    ASSERT_EQUALS(oneLong, oneDouble);
    ASSERT_EQUALS(oneDouble, one);

    const SafeNum oneDecimal(Decimal128(1));
    ASSERT_EQUALS(oneDecimal, one);
}

TEST(Comparison, MaxIntInDouble) {
    const SafeNum okToConvert(SafeNum::maxIntInDouble - 1);
    ASSERT_EQUALS(okToConvert, SafeNum(SafeNum::maxIntInDouble - 1.0));

    const SafeNum unsafeToConvert(SafeNum::maxIntInDouble + 100);
    ASSERT_NOT_EQUALS(unsafeToConvert, SafeNum(SafeNum::maxIntInDouble + 100.0));
}

TEST(Addition, Zero) {
    const SafeNum zero(0);
    ASSERT_EQUALS(zero + 0, zero);
    ASSERT_EQUALS(zero + zero, zero);

    const SafeNum minusOne(-1);
    const SafeNum plusOne(1);
    ASSERT_EQUALS(minusOne + 1, zero);
    ASSERT_EQUALS(zero + -1, minusOne);
    ASSERT_EQUALS(plusOne + -1, zero);
    ASSERT_EQUALS(zero + 1, plusOne);
}

TEST(Addition, UpConvertion) {
    const SafeNum zeroInt32(0);
    const SafeNum zeroInt64(static_cast<int64_t>(0));
    const SafeNum zeroDouble(0.0);
    ASSERT_EQUALS((zeroInt32 + zeroInt64).type(), merizo::NumberLong);
    ASSERT_EQUALS((zeroInt64 + zeroInt32).type(), merizo::NumberLong);
    ASSERT_EQUALS((zeroInt32 + zeroDouble).type(), merizo::NumberDouble);
    ASSERT_EQUALS((zeroInt64 + zeroDouble).type(), merizo::NumberDouble);


    const SafeNum stillInt32(zeroInt32 + zeroInt32);
    const SafeNum stillInt64(zeroInt64 + zeroInt64);
    const SafeNum stillDouble(zeroDouble + zeroDouble);
    ASSERT_EQUALS(stillInt32.type(), merizo::NumberInt);
    ASSERT_EQUALS(stillInt64.type(), merizo::NumberLong);
    ASSERT_EQUALS(stillDouble.type(), merizo::NumberDouble);

    const SafeNum zeroDecimal(Decimal128(0));
    ASSERT_EQUALS((zeroInt64 + zeroDecimal).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroInt32 + zeroDecimal).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDouble + zeroDecimal).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDecimal + zeroInt32).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDecimal + zeroInt64).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDecimal + zeroDouble).type(), merizo::NumberDecimal);

    const SafeNum stillDecimal(zeroDecimal + zeroDecimal);
    ASSERT_EQUALS(stillDecimal.type(), merizo::NumberDecimal);
}

TEST(Addition, Overflow32to64) {
    const SafeNum maxInt32(std::numeric_limits<int32_t>::max());
    ASSERT_EQUALS(maxInt32.type(), merizo::NumberInt);

    const SafeNum int32PlusOne(maxInt32 + 1);
    ASSERT_EQUALS(int32PlusOne.type(), merizo::NumberLong);

    const SafeNum int32MinusOne(maxInt32 + -1);
    ASSERT_EQUALS(int32MinusOne.type(), merizo::NumberInt);

    const SafeNum longResult(std::numeric_limits<int32_t>::max() + static_cast<int64_t>(1));
    ASSERT_EQUALS(int32PlusOne, longResult);
}

TEST(Addition, Overflow64toDouble) {
    const SafeNum maxInt64(std::numeric_limits<int64_t>::max());
    ASSERT_EQUALS(maxInt64.type(), merizo::NumberLong);

    // We don't overflow int64 to double.
    const SafeNum int64PlusOne(maxInt64 + 1);
    ASSERT_EQUALS(int64PlusOne.type(), merizo::EOO);

    const SafeNum int64MinusOne(maxInt64 + -1);
    ASSERT_EQUALS(int64MinusOne.type(), merizo::NumberLong);

    const SafeNum doubleResult(std::numeric_limits<int64_t>::max() + static_cast<double>(1));
    ASSERT_EQUALS(doubleResult.type(), merizo::NumberDouble);
    ASSERT_NOT_EQUALS(int64PlusOne, doubleResult);
}

TEST(Addition, OverflowDouble) {
    const SafeNum maxDouble(std::numeric_limits<double>::max());
    ASSERT_EQUALS(maxDouble.type(), merizo::NumberDouble);

    // can't just add one here, as max double is so sparse max == max+1
    const SafeNum doublePlusMax(maxDouble + maxDouble);
    ASSERT_EQUALS(doublePlusMax.type(), merizo::NumberDouble);

    const SafeNum infinity(std::numeric_limits<double>::infinity());
    ASSERT_EQUALS(doublePlusMax, infinity);
}

TEST(Addition, Negative32to64) {
    const SafeNum minInt32(std::numeric_limits<int32_t>::min());
    ASSERT_EQUALS(minInt32.type(), merizo::NumberInt);

    const SafeNum int32MinusOne(minInt32 + -1);
    ASSERT_EQUALS(int32MinusOne.type(), merizo::NumberLong);

    const SafeNum int32PlusOne(minInt32 + 1);
    ASSERT_EQUALS(int32PlusOne.type(), merizo::NumberInt);

    const SafeNum longResult(std::numeric_limits<int32_t>::min() - static_cast<int64_t>(1));
    ASSERT_EQUALS(int32MinusOne, longResult);
}

TEST(Addition, Negative64toDouble) {
    const SafeNum minInt64(std::numeric_limits<int64_t>::min());
    ASSERT_EQUALS(minInt64.type(), merizo::NumberLong);

    // We don't overflow int64 to double.
    const SafeNum int64MinusOne(minInt64 + -1);
    ASSERT_EQUALS(int64MinusOne.type(), merizo::EOO);

    const SafeNum int64PlusOne(minInt64 + 1);
    ASSERT_EQUALS(int64PlusOne.type(), merizo::NumberLong);

    const SafeNum doubleResult(std::numeric_limits<int64_t>::min() - static_cast<double>(1));
    ASSERT_EQUALS(doubleResult.type(), merizo::NumberDouble);
    ASSERT_NOT_EQUALS(int64MinusOne, doubleResult);
}

TEST(BitAnd, FloatingPointIsIgnored) {
    const SafeNum val_int(static_cast<int32_t>(1));
    const SafeNum val_ll(static_cast<int64_t>(1));
    const SafeNum val_double(1.0);
    ASSERT_FALSE((val_int & val_double).isValid());
    ASSERT_FALSE((val_double & val_int).isValid());
    ASSERT_FALSE((val_ll & val_double).isValid());
    ASSERT_FALSE((val_double & val_ll).isValid());
    ASSERT_FALSE((val_double & val_double).isValid());

    const SafeNum val_decimal(Decimal128(1));
    ASSERT_FALSE((val_int & val_decimal).isValid());
    ASSERT_FALSE((val_double & val_decimal).isValid());
    ASSERT_FALSE((val_ll & val_decimal).isValid());
    ASSERT_FALSE((val_decimal & val_int).isValid());
    ASSERT_FALSE((val_decimal & val_ll).isValid());
    ASSERT_FALSE((val_decimal & val_double).isValid());
}

TEST(BitAnd, 32and32) {
    const SafeNum val1(static_cast<int32_t>(0xE0F1));
    const SafeNum val2(static_cast<int32_t>(0xDF01));
    const SafeNum expected(static_cast<int32_t>(0xC001));
    const SafeNum result = val1 & val2;
    ASSERT_EQUALS(merizo::NumberInt, result.type());

    ASSERT_TRUE(expected.isIdentical(result));
}

TEST(BitAnd, 64and64) {
    const SafeNum val1(static_cast<int64_t>(0xE0F1E0F1E0F1));
    const SafeNum val2(static_cast<int64_t>(0xDF01DF01DF01));
    const SafeNum expected(static_cast<int64_t>(0xC001C001C001));
    const SafeNum result = val1 & val2;
    ASSERT_EQUALS(merizo::NumberLong, result.type());
    ASSERT_TRUE(expected.isIdentical(result));
}

TEST(BitAnd, MixedSize) {
    const SafeNum val_small(static_cast<int32_t>(0xE0F1));
    const SafeNum val_big(static_cast<int64_t>(0xDF01));
    const SafeNum expected(static_cast<int64_t>(0xC001));
    const SafeNum result_s_b = val_small & val_big;
    const SafeNum result_b_s = val_big & val_small;

    ASSERT_EQUALS(merizo::NumberLong, result_s_b.type());
    ASSERT_TRUE(expected.isIdentical(result_s_b));

    ASSERT_EQUALS(merizo::NumberLong, result_b_s.type());
    ASSERT_TRUE(expected.isIdentical(result_b_s));
}

TEST(BitOr, FloatingPointIsIgnored) {
    const SafeNum val_int(static_cast<int32_t>(1));
    const SafeNum val_ll(static_cast<int64_t>(1));
    const SafeNum val_double(1.0);
    ASSERT_FALSE((val_int | val_double).isValid());
    ASSERT_FALSE((val_double | val_int).isValid());
    ASSERT_FALSE((val_ll | val_double).isValid());
    ASSERT_FALSE((val_double | val_ll).isValid());
    ASSERT_FALSE((val_double | val_double).isValid());

    const SafeNum val_decimal(Decimal128(1));
    ASSERT_FALSE((val_decimal | val_int).isValid());
    ASSERT_FALSE((val_decimal | val_double).isValid());
    ASSERT_FALSE((val_decimal | val_ll).isValid());
    ASSERT_FALSE((val_int | val_decimal).isValid());
    ASSERT_FALSE((val_ll | val_decimal).isValid());
    ASSERT_FALSE((val_double | val_decimal).isValid());
}

TEST(BitOr, 32and32) {
    const SafeNum val1(static_cast<int32_t>(0xE0F1));
    const SafeNum val2(static_cast<int32_t>(0xDF01));
    const SafeNum result = val1 | val2;
    const SafeNum expected(static_cast<int32_t>(0xFFF1));
    ASSERT_EQUALS(merizo::NumberInt, result.type());
    ASSERT_TRUE(expected.isIdentical(result));
}

TEST(BitOr, 64and64) {
    const SafeNum val1(static_cast<int64_t>(0xE0F1E0F1E0F1));
    const SafeNum val2(static_cast<int64_t>(0xDF01DF01DF01));
    const SafeNum result = val1 | val2;
    const SafeNum expected(static_cast<int64_t>(0xFFF1FFF1FFF1));
    ASSERT_EQUALS(merizo::NumberLong, result.type());
    ASSERT_TRUE(expected.isIdentical(result));
}

TEST(BitOr, MixedSize) {
    const SafeNum val_small(static_cast<int32_t>(0xE0F1));
    const SafeNum val_big(static_cast<int64_t>(0xDF01));
    const SafeNum expected(static_cast<int64_t>(0xFFF1));
    const SafeNum result_s_b = val_small | val_big;
    const SafeNum result_b_s = val_big | val_small;

    ASSERT_EQUALS(merizo::NumberLong, result_s_b.type());
    ASSERT_TRUE(expected.isIdentical(result_s_b));

    ASSERT_EQUALS(merizo::NumberLong, result_b_s.type());
    ASSERT_TRUE(expected.isIdentical(result_b_s));
}

TEST(BitXor, FloatingPointIsIgnored) {
    const SafeNum val_int(static_cast<int32_t>(1));
    const SafeNum val_ll(static_cast<int64_t>(1));
    const SafeNum val_double(1.0);
    ASSERT_FALSE((val_int ^ val_double).isValid());
    ASSERT_FALSE((val_double ^ val_int).isValid());
    ASSERT_FALSE((val_ll ^ val_double).isValid());
    ASSERT_FALSE((val_double ^ val_ll).isValid());
    ASSERT_FALSE((val_double ^ val_double).isValid());

    const SafeNum val_decimal(Decimal128(1));
    ASSERT_FALSE((val_decimal ^ val_int).isValid());
    ASSERT_FALSE((val_decimal ^ val_ll).isValid());
    ASSERT_FALSE((val_decimal ^ val_double).isValid());
    ASSERT_FALSE((val_int ^ val_decimal).isValid());
    ASSERT_FALSE((val_ll ^ val_decimal).isValid());
    ASSERT_FALSE((val_double ^ val_decimal).isValid());
}

TEST(BitXor, 32and32) {
    const SafeNum val1(static_cast<int32_t>(0xE0F1));
    const SafeNum val2(static_cast<int32_t>(0xDF01));
    const SafeNum result = val1 ^ val2;
    const SafeNum expected(static_cast<int32_t>(0x3FF0));
    ASSERT_EQUALS(merizo::NumberInt, result.type());
    ASSERT_TRUE(expected.isIdentical(result));
}

TEST(BitXor, 64and64) {
    const SafeNum val1(static_cast<int64_t>(0xE0F1E0F1E0F1));
    const SafeNum val2(static_cast<int64_t>(0xDF01DF01DF01));
    const SafeNum result = val1 ^ val2;
    const SafeNum expected(static_cast<int64_t>(0x3FF03FF03FF0));
    ASSERT_EQUALS(merizo::NumberLong, result.type());
    ASSERT_TRUE(expected.isIdentical(result));
}

TEST(BitXor, MixedSize) {
    const SafeNum val_small(static_cast<int32_t>(0xE0F1));
    const SafeNum val_big(static_cast<int64_t>(0xDF01));
    const SafeNum expected(static_cast<int64_t>(0x3FF0));
    const SafeNum result_s_b = val_small ^ val_big;
    const SafeNum result_b_s = val_big ^ val_small;

    ASSERT_EQUALS(merizo::NumberLong, result_s_b.type());
    ASSERT_TRUE(expected.isIdentical(result_s_b));

    ASSERT_EQUALS(merizo::NumberLong, result_b_s.type());
    ASSERT_TRUE(expected.isIdentical(result_b_s));
}

TEST(Multiplication, Zero) {
    const SafeNum zero(0);
    ASSERT_EQUALS(zero * 0, zero);
    ASSERT_EQUALS(zero * zero, zero);
}

TEST(Multiplication, LongZero) {
    const SafeNum zero(static_cast<int64_t>(0));
    ASSERT_EQUALS(zero * static_cast<int64_t>(0), zero);
    ASSERT_EQUALS(zero * zero, zero);
}

TEST(Multiplication, DoubleZero) {
    const SafeNum zero(0.0);
    ASSERT_EQUALS(zero * 0.0, zero);
    ASSERT_EQUALS(zero * zero, zero);
}

TEST(Multiplication, One) {
    const SafeNum plusOne(1);
    ASSERT_EQUALS(plusOne * 1, plusOne);
    ASSERT_EQUALS(plusOne * plusOne, plusOne);
}

TEST(Multiplication, LongOne) {
    const SafeNum plusOne(static_cast<int64_t>(1));
    ASSERT_EQUALS(plusOne * static_cast<int64_t>(1), plusOne);
    ASSERT_EQUALS(plusOne * plusOne, plusOne);
}

TEST(Multiplication, DoubleOne) {
    const SafeNum plusOne(1.0);
    ASSERT_EQUALS(plusOne * 1.0, plusOne);
    ASSERT_EQUALS(plusOne * plusOne, plusOne);
}

TEST(Multiplication, UpConvertion) {
    const SafeNum zeroInt32(0);
    const SafeNum zeroInt64(static_cast<int64_t>(0));
    const SafeNum zeroDouble(0.0);
    ASSERT_EQUALS((zeroInt32 * zeroInt64).type(), merizo::NumberLong);
    ASSERT_EQUALS((zeroInt64 * zeroInt32).type(), merizo::NumberLong);
    ASSERT_EQUALS((zeroInt32 * zeroDouble).type(), merizo::NumberDouble);
    ASSERT_EQUALS((zeroInt64 * zeroDouble).type(), merizo::NumberDouble);
    ASSERT_EQUALS((zeroDouble * zeroInt32).type(), merizo::NumberDouble);
    ASSERT_EQUALS((zeroDouble * zeroInt64).type(), merizo::NumberDouble);

    const SafeNum stillInt32(zeroInt32 * zeroInt32);
    const SafeNum stillInt64(zeroInt64 * zeroInt64);
    const SafeNum stillDouble(zeroDouble * zeroDouble);
    ASSERT_EQUALS(stillInt32.type(), merizo::NumberInt);
    ASSERT_EQUALS(stillInt64.type(), merizo::NumberLong);
    ASSERT_EQUALS(stillDouble.type(), merizo::NumberDouble);

    const SafeNum zeroDecimal(Decimal128(0));
    ASSERT_EQUALS((zeroDecimal * zeroInt32).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroInt32 * zeroDecimal).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDecimal * zeroInt64).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroInt64 * zeroDecimal).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDecimal * zeroDouble).type(), merizo::NumberDecimal);
    ASSERT_EQUALS((zeroDouble * zeroDecimal).type(), merizo::NumberDecimal);
    const SafeNum stillDecimal(zeroDecimal * zeroDecimal);
    ASSERT_EQUALS(stillDecimal.type(), merizo::NumberDecimal);
}

TEST(Multiplication, Overflow32to64) {
    const SafeNum maxInt32(std::numeric_limits<int32_t>::max());
    ASSERT_EQUALS(maxInt32.type(), merizo::NumberInt);

    const SafeNum int32TimesOne(maxInt32 * 1);
    ASSERT_EQUALS(int32TimesOne.type(), merizo::NumberInt);

    const SafeNum int32TimesTwo(maxInt32 * 2);
    ASSERT_EQUALS(int32TimesTwo.type(), merizo::NumberLong);
}

TEST(Multiplication, Overflow64toDouble) {
    const SafeNum maxInt64(std::numeric_limits<int64_t>::max());
    ASSERT_EQUALS(maxInt64.type(), merizo::NumberLong);

    // We don't overflow int64 to double.
    const SafeNum int64TimesTwo(maxInt64 * 2);
    ASSERT_EQUALS(int64TimesTwo.type(), merizo::EOO);

    const SafeNum doubleResult(std::numeric_limits<int64_t>::max() * static_cast<double>(2));
    ASSERT_EQUALS(doubleResult.type(), merizo::NumberDouble);
    ASSERT_NOT_EQUALS(int64TimesTwo, doubleResult);
}

TEST(Multiplication, OverflowDouble) {
    const SafeNum maxDouble(std::numeric_limits<double>::max());
    ASSERT_EQUALS(maxDouble.type(), merizo::NumberDouble);

    const SafeNum doublePlusMax(maxDouble * maxDouble);
    ASSERT_EQUALS(doublePlusMax.type(), merizo::NumberDouble);

    const SafeNum infinity(std::numeric_limits<double>::infinity());
    ASSERT_EQUALS(doublePlusMax, infinity);
}

TEST(Multiplication, Negative32to64) {
    const SafeNum minInt32(std::numeric_limits<int32_t>::min());
    ASSERT_EQUALS(minInt32.type(), merizo::NumberInt);

    const SafeNum int32TimesOne(minInt32 * 1);
    ASSERT_EQUALS(int32TimesOne.type(), merizo::NumberInt);

    const SafeNum int32TimesTwo(minInt32 * 2);
    ASSERT_EQUALS(int32TimesTwo.type(), merizo::NumberLong);
}

TEST(Multiplication, Negative64toDouble) {
    const SafeNum minInt64(std::numeric_limits<int64_t>::min());
    ASSERT_EQUALS(minInt64.type(), merizo::NumberLong);

    // We don't overflow int64 to double.
    const SafeNum int64TimesTwo(minInt64 * 2);
    ASSERT_EQUALS(int64TimesTwo.type(), merizo::EOO);

    const SafeNum int64TimesOne(minInt64 * 1);
    ASSERT_EQUALS(int64TimesOne.type(), merizo::NumberLong);

    const SafeNum doubleResult(std::numeric_limits<int64_t>::min() * static_cast<double>(2));
    ASSERT_EQUALS(doubleResult.type(), merizo::NumberDouble);
    ASSERT_NOT_EQUALS(int64TimesTwo, doubleResult);
}

TEST(Multiplication, 64OverflowsFourWays) {
    const SafeNum maxInt64(std::numeric_limits<int64_t>::max());
    const SafeNum minInt64(std::numeric_limits<int64_t>::min());
    ASSERT_EQUALS(merizo::EOO, (maxInt64 * maxInt64).type());
    ASSERT_EQUALS(merizo::EOO, (maxInt64 * minInt64).type());
    ASSERT_EQUALS(merizo::EOO, (minInt64 * maxInt64).type());
    ASSERT_EQUALS(merizo::EOO, (minInt64 * minInt64).type());
}

TEST(Multiplication, BoundsWithNegativeOne) {
    const SafeNum maxInt64(std::numeric_limits<int64_t>::max());
    const SafeNum minInt64(std::numeric_limits<int64_t>::min());
    const SafeNum minusOneInt64(static_cast<int64_t>(-1));
    ASSERT_NOT_EQUALS(merizo::EOO, (maxInt64 * minusOneInt64).type());
    ASSERT_NOT_EQUALS(merizo::EOO, (minusOneInt64 * maxInt64).type());
    ASSERT_EQUALS(merizo::EOO, (minInt64 * minusOneInt64).type());
    ASSERT_EQUALS(merizo::EOO, (minusOneInt64 * minInt64).type());
}

}  // unnamed namespace
