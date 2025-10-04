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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cmath>
#include <memory>


namespace mongo {


namespace {
const double DELTA = 0.0001;

void testEquals(Value actual, Value expected, double delta = DELTA) {
    if (actual.getType() == BSONType::numberDouble || actual.getType() == BSONType::numberDouble) {
        ASSERT_APPROX_EQUAL(actual.coerceToDouble(), expected.coerceToDouble(), delta);
    } else {
        ASSERT_VALUE_EQ(actual, expected);
    }
}

TEST(GranularityRounderPowersOfTwoTest, ShouldRoundUpPowersOfTwoToNextPowerOfTwo) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    testEquals(rounder->roundUp(Value(0.5)), Value(1));
    testEquals(rounder->roundUp(Value(1)), Value(2));
    testEquals(rounder->roundUp(Value(2)), Value(4));
    testEquals(rounder->roundUp(Value(4)), Value(8));
    testEquals(rounder->roundUp(Value(8)), Value(16));

    long long input = 2305843009213693952;   // 2^61
    long long output = 4611686018427387904;  // 2^62
    testEquals(rounder->roundUp(Value(input)), Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldReturnDoubleIfExceedsNumberLong) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    long long input = 4611686018427387905;  // 2^62 + 1
    double output = 9223372036854775808.0;  // 2^63

    Value inputValue = Value(input);
    ASSERT_EQ(inputValue.getType(), BSONType::numberLong);

    Value roundedValue = rounder->roundUp(inputValue);
    ASSERT_EQ(roundedValue.getType(), BSONType::numberDouble);
    testEquals(roundedValue, Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldReturnNumberLongIfExceedsNumberInt) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    int input = 1073741824;         // 2^30
    long long output = 2147483648;  // 2^31

    Value inputValue = Value(input);
    ASSERT_EQ(inputValue.getType(), BSONType::numberInt);

    Value roundedValue = rounder->roundUp(inputValue);
    ASSERT_EQ(roundedValue.getType(), BSONType::numberLong);
    testEquals(roundedValue, Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldReturnNumberLongIfRoundedDownDoubleIsSmallEnough) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    double input = 9223372036854775808.0;    // 2^63
    long long output = 4611686018427387904;  // 2^62

    Value inputValue = Value(input);
    ASSERT_EQ(inputValue.getType(), BSONType::numberDouble);

    Value roundedValue = rounder->roundDown(inputValue);
    ASSERT_EQ(roundedValue.getType(), BSONType::numberLong);
    testEquals(roundedValue, Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldReturnNumberIntIfRoundedDownNumberLongIsSmallEnough) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    long long input = 2147483648;  // 2^31
    int output = 1073741824;       // 2^30

    Value inputValue = Value(input);
    ASSERT_EQ(inputValue.getType(), BSONType::numberLong);

    Value roundedValue = rounder->roundDown(inputValue);
    ASSERT_EQ(roundedValue.getType(), BSONType::numberInt);
    testEquals(roundedValue, Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldReturnNumberDecimalWhenRoundingUpNumberDecimal) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    Decimal128 input = Decimal128(0.12);
    Decimal128 output = Decimal128(0.125);

    Value inputValue = Value(input);
    ASSERT_EQ(inputValue.getType(), BSONType::numberDecimal);

    Value roundedValue = rounder->roundUp(inputValue);
    ASSERT_EQ(roundedValue.getType(), BSONType::numberDecimal);
    testEquals(roundedValue, Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldReturnNumberDecimalWhenRoundingDownNumberDecimal) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    Decimal128 input = Decimal128(0.13);
    Decimal128 output = Decimal128(0.125);

    Value inputValue = Value(input);
    ASSERT_EQ(inputValue.getType(), BSONType::numberDecimal);

    Value roundedValue = rounder->roundDown(inputValue);
    ASSERT_EQ(roundedValue.getType(), BSONType::numberDecimal);
    testEquals(roundedValue, Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldRoundUpNonPowersOfTwoToNextPowerOfTwo) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    testEquals(rounder->roundUp(Value(3)), Value(4));
    testEquals(rounder->roundUp(Value(5)), Value(8));
    testEquals(rounder->roundUp(Value(6)), Value(8));
    testEquals(rounder->roundUp(Value(7)), Value(8));

    testEquals(rounder->roundUp(Value(0.1)), Value(0.125));
    testEquals(rounder->roundUp(Value(0.2)), Value(0.25));
    testEquals(rounder->roundUp(Value(0.3)), Value(0.5));
    testEquals(rounder->roundUp(Value(1.5)), Value(2));
    testEquals(rounder->roundUp(Value(3.6)), Value(4));
    testEquals(rounder->roundUp(Value(5.7)), Value(8));

    long long input = 4611686018427387903;   // 2^62 -1
    long long output = 4611686018427387904;  // 2^62
    testEquals(rounder->roundUp(Value(input)), Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldRoundDownPowersOfTwoToNextPowerOfTwo) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    testEquals(rounder->roundDown(Value(16)), Value(8));
    testEquals(rounder->roundDown(Value(8)), Value(4));
    testEquals(rounder->roundDown(Value(4)), Value(2));
    testEquals(rounder->roundDown(Value(2)), Value(1));
    testEquals(rounder->roundDown(Value(1)), Value(0.5));
    testEquals(rounder->roundDown(Value(0.5)), Value(0.25));

    long long input = 4611686018427387904;   // 2^62
    long long output = 2305843009213693952;  // 2^61
    testEquals(rounder->roundDown(Value(input)), Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldRoundDownNonPowersOfTwoToNextPowerOfTwo) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    testEquals(rounder->roundDown(Value(10)), Value(8));
    testEquals(rounder->roundDown(Value(9)), Value(8));
    testEquals(rounder->roundDown(Value(7)), Value(4));
    testEquals(rounder->roundDown(Value(6)), Value(4));
    testEquals(rounder->roundDown(Value(5)), Value(4));
    testEquals(rounder->roundDown(Value(3)), Value(2));
    testEquals(rounder->roundDown(Value(0.7)), Value(0.5));
    testEquals(rounder->roundDown(Value(0.4)), Value(0.25));
    testEquals(rounder->roundDown(Value(0.17)), Value(0.125));

    long long input = 4611686018427387905;   // 2^62 + 1
    long long output = 4611686018427387904;  // 2^62
    testEquals(rounder->roundDown(Value(input)), Value(output));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldRoundZeroToZero) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    testEquals(rounder->roundUp(Value(0)), Value(0));
    testEquals(rounder->roundDown(Value(0)), Value(0));
}

TEST(GranularityRounderPowersOfTwoTest, ShouldFailOnRoundingNonNumericValues) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    // Make sure that each GranularityRounder fails when rounding a non-numeric value.
    Value stringValue = Value("test"_sd);
    ASSERT_THROWS_CODE(rounder->roundUp(stringValue), AssertionException, 40265);
    ASSERT_THROWS_CODE(rounder->roundDown(stringValue), AssertionException, 40265);
}

TEST(GranularityRounderPowersOfTwoTest, ShouldFailOnRoundingNaN) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    Value nan = Value(std::nan("NaN"));
    ASSERT_THROWS_CODE(rounder->roundUp(nan), AssertionException, 40266);
    ASSERT_THROWS_CODE(rounder->roundDown(nan), AssertionException, 40266);

    Value positiveNan = Value(Decimal128::kPositiveNaN);
    Value negativeNan = Value(Decimal128::kNegativeNaN);
    ASSERT_THROWS_CODE(rounder->roundUp(positiveNan), AssertionException, 40266);
    ASSERT_THROWS_CODE(rounder->roundDown(positiveNan), AssertionException, 40266);
    ASSERT_THROWS_CODE(rounder->roundUp(negativeNan), AssertionException, 40266);
    ASSERT_THROWS_CODE(rounder->roundDown(negativeNan), AssertionException, 40266);
}

TEST(GranularityRounderPowersOfTwoTest, ShouldFailOnRoundingNegativeNumber) {
    auto rounder =
        GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), "POWERSOF2");

    Value negativeNumber = Value(-1);
    ASSERT_THROWS_CODE(rounder->roundUp(negativeNumber), AssertionException, 40267);
    ASSERT_THROWS_CODE(rounder->roundDown(negativeNumber), AssertionException, 40267);
}
}  // namespace
}  // namespace mongo
