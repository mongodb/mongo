/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

} // unnamed namespace
