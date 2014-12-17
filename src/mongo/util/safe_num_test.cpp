/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <limits>

#include "mongo/pch.h" // for malloc/realloc pulled from bson

#include "mongo/bson/bsontypes.h"
#include "mongo/util/safe_num.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::SafeNum;

    TEST(Basics, Initialization) {
        const SafeNum numInt(0);
        ASSERT_EQUALS(numInt.type(), mongo::NumberInt);

        const SafeNum numLong(0LL);
        ASSERT_EQUALS(numLong.type(), mongo::NumberLong);

        const SafeNum numDouble(0.0);
        ASSERT_EQUALS(numDouble.type(), mongo::NumberDouble);
    }

    TEST(Comparison, EOO) {
        const SafeNum safeNumA;
        const SafeNum safeNumB;
        ASSERT_FALSE(safeNumA.isValid());
        ASSERT_FALSE(safeNumB.isValid());
        ASSERT_EQUALS(safeNumA.type(), mongo::EOO);
        ASSERT_EQUALS(safeNumB.type(), mongo::EOO);
        ASSERT_TRUE(safeNumA.isEquivalent(safeNumB));
        ASSERT_FALSE(safeNumA.isIdentical(safeNumB));

        const SafeNum one(1);
        ASSERT_NOT_EQUALS(one, safeNumA);
    }

    TEST(Comparison, StrictTypeComparison) {
        const SafeNum one(1);
        const SafeNum oneLong(1LL);
        const SafeNum oneDouble(1.0);
        ASSERT_FALSE(one.isIdentical(oneLong));
        ASSERT_FALSE(oneLong.isIdentical(oneDouble));
        ASSERT_FALSE(oneDouble.isIdentical(one));
    }

    TEST(Comparison, EquivalenceComparisonNormal) {
        const SafeNum one(1);
        const SafeNum oneLong(1LL);
        const SafeNum oneDouble(1.0);
        ASSERT_EQUALS(one, oneLong);
        ASSERT_EQUALS(oneLong, oneDouble);
        ASSERT_EQUALS(oneDouble, one);
    }

    TEST(Comparison, MaxIntInDouble) {
        const SafeNum okToConvert(SafeNum::maxIntInDouble-1);
        ASSERT_EQUALS(okToConvert, SafeNum(SafeNum::maxIntInDouble-1.0));

        const SafeNum unsafeToConvert(SafeNum::maxIntInDouble + 100);
        ASSERT_NOT_EQUALS(unsafeToConvert, SafeNum(SafeNum::maxIntInDouble+100.0));
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
        const SafeNum zeroInt64(0LL);
        const SafeNum zeroDouble(0.0);
        ASSERT_EQUALS((zeroInt32 + zeroInt64).type(), mongo::NumberLong);
        ASSERT_EQUALS((zeroInt64 + zeroInt32).type(), mongo::NumberLong);
        ASSERT_EQUALS((zeroInt32 + zeroDouble).type(), mongo::NumberDouble);
        ASSERT_EQUALS((zeroInt64 + zeroDouble).type(), mongo::NumberDouble);

        const SafeNum stillInt32(zeroInt32 + zeroInt32);
        const SafeNum stillInt64(zeroInt64 + zeroInt64);
        const SafeNum stillDouble(zeroDouble + zeroDouble);
        ASSERT_EQUALS(stillInt32.type(), mongo::NumberInt);
        ASSERT_EQUALS(stillInt64.type(), mongo::NumberLong);
        ASSERT_EQUALS(stillDouble.type(), mongo::NumberDouble);
    }

    TEST(Addition, Overflow32to64) {
        const SafeNum maxInt32(std::numeric_limits<int>::max());
        ASSERT_EQUALS(maxInt32.type(), mongo::NumberInt);

        const SafeNum int32PlusOne(maxInt32 + 1);
        ASSERT_EQUALS(int32PlusOne.type(), mongo::NumberLong);

        const SafeNum int32MinusOne(maxInt32 + -1);
        ASSERT_EQUALS(int32MinusOne.type(), mongo::NumberInt);

        const SafeNum longResult(std::numeric_limits<int>::max() + static_cast<long long>(1));
        ASSERT_EQUALS(int32PlusOne, longResult);
    }

    TEST(Addition, Overflow64toDouble) {
        const SafeNum maxInt64(std::numeric_limits<long long>::max());
        ASSERT_EQUALS(maxInt64.type(), mongo::NumberLong);

        // We don't overflow int64 to double.
        const SafeNum int64PlusOne(maxInt64 + 1);
        ASSERT_EQUALS(int64PlusOne.type(), mongo::EOO);

        const SafeNum int64MinusOne(maxInt64 + -1);
        ASSERT_EQUALS(int64MinusOne.type(), mongo::NumberLong);

        const SafeNum doubleResult(std::numeric_limits<long long>::max()+static_cast<double>(1));
        ASSERT_EQUALS(doubleResult.type(), mongo::NumberDouble);
        ASSERT_NOT_EQUALS(int64PlusOne, doubleResult);
    }

    TEST(Addition, OverflowDouble) {
        const SafeNum maxDouble(std::numeric_limits<double>::max());
        ASSERT_EQUALS(maxDouble.type(), mongo::NumberDouble);

        // can't just add one here, as max double is so sparse max == max+1
        const SafeNum doublePlusMax(maxDouble + maxDouble);
        ASSERT_EQUALS(doublePlusMax.type(), mongo::NumberDouble);

        const SafeNum infinity(std::numeric_limits<double>::infinity());
        ASSERT_EQUALS(doublePlusMax, infinity);
    }

    TEST(Addition, Negative32to64) {
        const SafeNum minInt32(std::numeric_limits<int>::min());
        ASSERT_EQUALS(minInt32.type(), mongo::NumberInt);

        const SafeNum int32MinusOne(minInt32 + -1);
        ASSERT_EQUALS(int32MinusOne.type(), mongo::NumberLong);

        const SafeNum int32PlusOne(minInt32 + 1);
        ASSERT_EQUALS(int32PlusOne.type(), mongo::NumberInt);

        const SafeNum longResult(std::numeric_limits<int>::min()-static_cast<long long>(1));
        ASSERT_EQUALS(int32MinusOne, longResult);
    }

    TEST(Addition, Negative64toDouble) {
        const SafeNum minInt64(std::numeric_limits<long long>::min());
        ASSERT_EQUALS(minInt64.type(), mongo::NumberLong);

        // We don't overflow int64 to double.
        const SafeNum int64MinusOne(minInt64 + -1);
        ASSERT_EQUALS(int64MinusOne.type(), mongo::EOO);

        const SafeNum int64PlusOne(minInt64 + 1);
        ASSERT_EQUALS(int64PlusOne.type(), mongo::NumberLong);

        const SafeNum doubleResult(std::numeric_limits<long long>::min()-static_cast<double>(1));
        ASSERT_EQUALS(doubleResult.type(), mongo::NumberDouble);
        ASSERT_NOT_EQUALS(int64MinusOne, doubleResult);
    }

    TEST(BitAnd, DoubleIsIgnored) {
        const SafeNum val_int(static_cast<int>(1));
        const SafeNum val_ll(static_cast<long long>(1));
        const SafeNum val_double(1.0);
        ASSERT_FALSE((val_int & val_double).isValid());
        ASSERT_FALSE((val_double & val_int).isValid());
        ASSERT_FALSE((val_ll & val_double).isValid());
        ASSERT_FALSE((val_double & val_ll).isValid());
        ASSERT_FALSE((val_double & val_double).isValid());
    }

    TEST(BitAnd, 32and32) {
        const SafeNum val1(static_cast<int>(0xE0F1U));
        const SafeNum val2(static_cast<int>(0xDF01U));
        const SafeNum expected(static_cast<int>(0xC001U));
        const SafeNum result = val1 & val2;
        ASSERT_EQUALS(mongo::NumberInt, result.type());

        ASSERT_TRUE(expected.isIdentical(result));
    }

    TEST(BitAnd, 64and64) {
        const SafeNum val1(static_cast<long long>(0xE0F1E0F1E0F1ULL));
        const SafeNum val2(static_cast<long long>(0xDF01DF01DF01ULL));
        const SafeNum expected(static_cast<long long>(0xC001C001C001ULL));
        const SafeNum result = val1 & val2;
        ASSERT_EQUALS(mongo::NumberLong, result.type());
        ASSERT_TRUE(expected.isIdentical(result));
    }

    TEST(BitAnd, MixedSize) {
        const SafeNum val_small(static_cast<int>(0xE0F1U));
        const SafeNum val_big(static_cast<long long>(0xDF01U));
        const SafeNum expected(static_cast<long long>(0xC001U));
        const SafeNum result_s_b = val_small & val_big;
        const SafeNum result_b_s = val_big & val_small;

        ASSERT_EQUALS(mongo::NumberLong, result_s_b.type());
        ASSERT_TRUE(expected.isIdentical(result_s_b));

        ASSERT_EQUALS(mongo::NumberLong, result_b_s.type());
        ASSERT_TRUE(expected.isIdentical(result_b_s));
    }

    TEST(BitOr, DoubleIsIgnored) {
        const SafeNum val_int(static_cast<int>(1));
        const SafeNum val_ll(static_cast<long long>(1));
        const SafeNum val_double(1.0);
        ASSERT_FALSE((val_int | val_double).isValid());
        ASSERT_FALSE((val_double | val_int).isValid());
        ASSERT_FALSE((val_ll | val_double).isValid());
        ASSERT_FALSE((val_double | val_ll).isValid());
        ASSERT_FALSE((val_double | val_double).isValid());
    }

    TEST(BitOr, 32and32) {
        const SafeNum val1(static_cast<int>(0xE0F1U));
        const SafeNum val2(static_cast<int>(0xDF01U));
        const SafeNum result = val1 | val2;
        const SafeNum expected(static_cast<int>(0xFFF1U));
        ASSERT_EQUALS(mongo::NumberInt, result.type());
        ASSERT_TRUE(expected.isIdentical(result));
    }

    TEST(BitOr, 64and64) {
        const SafeNum val1(static_cast<long long>(0xE0F1E0F1E0F1ULL));
        const SafeNum val2(static_cast<long long>(0xDF01DF01DF01ULL));
        const SafeNum result = val1 | val2;
        const SafeNum expected(static_cast<long long>(0xFFF1FFF1FFF1ULL));
        ASSERT_EQUALS(mongo::NumberLong, result.type());
        ASSERT_TRUE(expected.isIdentical(result));
    }

    TEST(BitOr, MixedSize) {
        const SafeNum val_small(static_cast<int>(0xE0F1U));
        const SafeNum val_big(static_cast<long long>(0xDF01U));
        const SafeNum expected(static_cast<long long>(0xFFF1U));
        const SafeNum result_s_b = val_small | val_big;
        const SafeNum result_b_s = val_big | val_small;

        ASSERT_EQUALS(mongo::NumberLong, result_s_b.type());
        ASSERT_TRUE(expected.isIdentical(result_s_b));

        ASSERT_EQUALS(mongo::NumberLong, result_b_s.type());
        ASSERT_TRUE(expected.isIdentical(result_b_s));
    }

    TEST(BitXor, DoubleIsIgnored) {
        const SafeNum val_int(static_cast<int>(1));
        const SafeNum val_ll(static_cast<long long>(1));
        const SafeNum val_double(1.0);
        ASSERT_FALSE((val_int ^ val_double).isValid());
        ASSERT_FALSE((val_double ^ val_int).isValid());
        ASSERT_FALSE((val_ll ^ val_double).isValid());
        ASSERT_FALSE((val_double ^ val_ll).isValid());
        ASSERT_FALSE((val_double ^ val_double).isValid());
    }

    TEST(BitXor, 32and32) {
        const SafeNum val1(static_cast<int>(0xE0F1U));
        const SafeNum val2(static_cast<int>(0xDF01U));
        const SafeNum result = val1 ^ val2;
        const SafeNum expected(static_cast<int>(0x3FF0U));
        ASSERT_EQUALS(mongo::NumberInt, result.type());
        ASSERT_TRUE(expected.isIdentical(result));
    }

    TEST(BitXor, 64and64) {
        const SafeNum val1(static_cast<long long>(0xE0F1E0F1E0F1ULL));
        const SafeNum val2(static_cast<long long>(0xDF01DF01DF01ULL));
        const SafeNum result = val1 ^ val2;
        const SafeNum expected(static_cast<long long>(0x3FF03FF03FF0ULL));
        ASSERT_EQUALS(mongo::NumberLong, result.type());
        ASSERT_TRUE(expected.isIdentical(result));
    }

    TEST(BitXor, MixedSize) {
        const SafeNum val_small(static_cast<int>(0xE0F1U));
        const SafeNum val_big(static_cast<long long>(0xDF01U));
        const SafeNum expected(static_cast<long long>(0x3FF0U));
        const SafeNum result_s_b = val_small ^ val_big;
        const SafeNum result_b_s = val_big ^ val_small;

        ASSERT_EQUALS(mongo::NumberLong, result_s_b.type());
        ASSERT_TRUE(expected.isIdentical(result_s_b));

        ASSERT_EQUALS(mongo::NumberLong, result_b_s.type());
        ASSERT_TRUE(expected.isIdentical(result_b_s));
    }

    TEST(Multiplication, Zero) {
        const SafeNum zero(0);
        ASSERT_EQUALS(zero * 0, zero);
        ASSERT_EQUALS(zero * zero, zero);
    }

    TEST(Multiplication, LongZero) {
        const SafeNum zero(0LL);
        ASSERT_EQUALS(zero * 0LL, zero);
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
        const SafeNum plusOne(1LL);
        ASSERT_EQUALS(plusOne * 1LL, plusOne);
        ASSERT_EQUALS(plusOne * plusOne, plusOne);
    }

    TEST(Multiplication, DoubleOne) {
        const SafeNum plusOne(1.0);
        ASSERT_EQUALS(plusOne * 1.0, plusOne);
        ASSERT_EQUALS(plusOne * plusOne, plusOne);
    }

    TEST(Multiplication, UpConvertion) {
        const SafeNum zeroInt32(0);
        const SafeNum zeroInt64(0LL);
        const SafeNum zeroDouble(0.0);
        ASSERT_EQUALS((zeroInt32 * zeroInt64).type(), mongo::NumberLong);
        ASSERT_EQUALS((zeroInt64 * zeroInt32).type(), mongo::NumberLong);
        ASSERT_EQUALS((zeroInt32 * zeroDouble).type(), mongo::NumberDouble);
        ASSERT_EQUALS((zeroInt64 * zeroDouble).type(), mongo::NumberDouble);
        ASSERT_EQUALS((zeroDouble * zeroInt32).type(), mongo::NumberDouble);
        ASSERT_EQUALS((zeroDouble * zeroInt64).type(), mongo::NumberDouble);

        const SafeNum stillInt32(zeroInt32 * zeroInt32);
        const SafeNum stillInt64(zeroInt64 * zeroInt64);
        const SafeNum stillDouble(zeroDouble * zeroDouble);
        ASSERT_EQUALS(stillInt32.type(), mongo::NumberInt);
        ASSERT_EQUALS(stillInt64.type(), mongo::NumberLong);
        ASSERT_EQUALS(stillDouble.type(), mongo::NumberDouble);
    }

    TEST(Multiplication, Overflow32to64) {
        const SafeNum maxInt32(std::numeric_limits<int>::max());
        ASSERT_EQUALS(maxInt32.type(), mongo::NumberInt);

        const SafeNum int32TimesOne(maxInt32 * 1);
        ASSERT_EQUALS(int32TimesOne.type(), mongo::NumberInt);

        const SafeNum int32TimesTwo(maxInt32 * 2);
        ASSERT_EQUALS(int32TimesTwo.type(), mongo::NumberLong);
    }

    TEST(Multiplication, Overflow64toDouble) {
        const SafeNum maxInt64(std::numeric_limits<long long>::max());
        ASSERT_EQUALS(maxInt64.type(), mongo::NumberLong);

        // We don't overflow int64 to double.
        const SafeNum int64TimesTwo(maxInt64 * 2);
        ASSERT_EQUALS(int64TimesTwo.type(), mongo::EOO);

        const SafeNum doubleResult(std::numeric_limits<long long>::max()*static_cast<double>(2));
        ASSERT_EQUALS(doubleResult.type(), mongo::NumberDouble);
        ASSERT_NOT_EQUALS(int64TimesTwo, doubleResult);
    }

    TEST(Multiplication, OverflowDouble) {
        const SafeNum maxDouble(std::numeric_limits<double>::max());
        ASSERT_EQUALS(maxDouble.type(), mongo::NumberDouble);

        const SafeNum doublePlusMax(maxDouble * maxDouble);
        ASSERT_EQUALS(doublePlusMax.type(), mongo::NumberDouble);

        const SafeNum infinity(std::numeric_limits<double>::infinity());
        ASSERT_EQUALS(doublePlusMax, infinity);
    }

    TEST(Multiplication, Negative32to64) {
        const SafeNum minInt32(std::numeric_limits<int>::min());
        ASSERT_EQUALS(minInt32.type(), mongo::NumberInt);

        const SafeNum int32TimesOne(minInt32 * 1);
        ASSERT_EQUALS(int32TimesOne.type(), mongo::NumberInt);

        const SafeNum int32TimesTwo(minInt32 * 2);
        ASSERT_EQUALS(int32TimesTwo.type(), mongo::NumberLong);
    }

    TEST(Multiplication, Negative64toDouble) {
        const SafeNum minInt64(std::numeric_limits<long long>::min());
        ASSERT_EQUALS(minInt64.type(), mongo::NumberLong);

        // We don't overflow int64 to double.
        const SafeNum int64TimesTwo(minInt64 * 2);
        ASSERT_EQUALS(int64TimesTwo.type(), mongo::EOO);

        const SafeNum int64TimesOne(minInt64 * 1);
        ASSERT_EQUALS(int64TimesOne.type(), mongo::NumberLong);

        const SafeNum doubleResult(std::numeric_limits<long long>::min()*static_cast<double>(2));
        ASSERT_EQUALS(doubleResult.type(), mongo::NumberDouble);
        ASSERT_NOT_EQUALS(int64TimesTwo, doubleResult);
    }

    TEST(Multiplication, 64OverflowsFourWays) {
        const SafeNum maxInt64(std::numeric_limits<long long>::max());
        const SafeNum minInt64(std::numeric_limits<long long>::min());
        ASSERT_EQUALS(mongo::EOO, (maxInt64 * maxInt64).type());
        ASSERT_EQUALS(mongo::EOO, (maxInt64 * minInt64).type());
        ASSERT_EQUALS(mongo::EOO, (minInt64 * maxInt64).type());
        ASSERT_EQUALS(mongo::EOO, (minInt64 * minInt64).type());
    }

    TEST(Multiplication, BoundsWithNegativeOne) {
        const SafeNum maxInt64(std::numeric_limits<long long>::max());
        const SafeNum minInt64(std::numeric_limits<long long>::min());
        const SafeNum minusOneInt64(-1LL);
        ASSERT_NOT_EQUALS(mongo::EOO, (maxInt64 * minusOneInt64).type());
        ASSERT_NOT_EQUALS(mongo::EOO, (minusOneInt64 * maxInt64).type());
        ASSERT_EQUALS(mongo::EOO, (minInt64 * minusOneInt64).type());
        ASSERT_EQUALS(mongo::EOO, (minusOneInt64 * minInt64).type());
    }

} // unnamed namespace
