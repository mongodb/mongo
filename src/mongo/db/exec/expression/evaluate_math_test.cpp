/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/summation.h"

#include <climits>
#include <cmath>
#include <limits>

namespace mongo {
namespace expression_evaluation_test {

using boost::intrusive_ptr;

/* ------------------------ ExpressionRange --------------------------- */

TEST(ExpressionRangeTest, ComputesStandardRange) {
    assertExpectedResults("$range", {{{Value(0), Value(3)}, Value(BSON_ARRAY(0 << 1 << 2))}});
}

TEST(ExpressionRangeTest, ComputesRangeWithStep) {
    assertExpectedResults("$range",
                          {{{Value(0), Value(6), Value(2)}, Value(BSON_ARRAY(0 << 2 << 4))}});
}

TEST(ExpressionRangeTest, ComputesReverseRange) {
    assertExpectedResults("$range",
                          {{{Value(0), Value(-3), Value(-1)}, Value(BSON_ARRAY(0 << -1 << -2))}});
}

TEST(ExpressionRangeTest, ComputesRangeWithPositiveAndNegative) {
    assertExpectedResults("$range",
                          {{{Value(-2), Value(3)}, Value(BSON_ARRAY(-2 << -1 << 0 << 1 << 2))}});
}

TEST(ExpressionRangeTest, ComputesEmptyRange) {
    assertExpectedResults("$range",
                          {{{Value(-2), Value(3), Value(-1)}, Value(std::vector<Value>())}});
}

TEST(ExpressionRangeTest, ComputesRangeWithSameStartAndEnd) {
    assertExpectedResults("$range", {{{Value(20), Value(20)}, Value(std::vector<Value>())}});
}

TEST(ExpressionRangeTest, ComputesRangeWithLargeNegativeStep) {
    assertExpectedResults("$range",
                          {{{Value(3), Value(-5), Value(-3)}, Value(BSON_ARRAY(3 << 0 << -3))}});
}

/* ------------------------ Add -------------------- */

TEST(ExpressionAddTest, NullDocument) {
    /** $add with a NULL Document pointer, as called by ExpressionNary::optimize().
     */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
    expression->addOperand(ExpressionConstant::create(&expCtx, Value(2)));
    ASSERT_BSONOBJ_EQ(BSON("" << 2), toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionAddTest, NoOperands) {
    /** $add without operands. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
    ASSERT_BSONOBJ_EQ(BSON("" << 0), toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionAddTest, String) {
    /** String type unsupported. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
    expression->addOperand(ExpressionConstant::create(&expCtx, Value("a"_sd)));
    ASSERT_THROWS(expression->evaluate({}, &expCtx.variables), AssertionException);
}

TEST(ExpressionAddTest, Bool) {
    /** Bool type unsupported. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
    expression->addOperand(ExpressionConstant::create(&expCtx, Value(true)));
    ASSERT_THROWS(expression->evaluate({}, &expCtx.variables), AssertionException);
}

namespace {
void testSingleArg(Value arg, Value result) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
    expression->addOperand(ExpressionConstant::create(&expCtx, arg));
    ASSERT_VALUE_EQ(result, expression->evaluate({}, &expCtx.variables));
}

void testDoubleArg(Value arg1, Value arg2, Value result) {
    {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
        expression->addOperand(ExpressionConstant::create(&expCtx, arg1));
        expression->addOperand(ExpressionConstant::create(&expCtx, arg2));
        ASSERT_VALUE_EQ(result, expression->evaluate({}, &expCtx.variables));
    }
    // Now add the operands in the reverse direction.
    {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
        expression->addOperand(ExpressionConstant::create(&expCtx, arg2));
        expression->addOperand(ExpressionConstant::create(&expCtx, arg1));
        ASSERT_VALUE_EQ(result, expression->evaluate({}, &expCtx.variables));
    }
}
}  // namespace

TEST(ExpressionAddTest, Int) {
    /** Single int argument. */
    testSingleArg(Value(1), Value(1));
}

TEST(ExpressionAddTest, Long) {
    /** Single long argument. */
    testSingleArg(Value(5555LL), Value(5555LL));
}

TEST(ExpressionAddTest, Double) {
    /** Single double argument. */
    testSingleArg(Value(99.99), Value(99.99));
}

TEST(ExpressionAddTest, Date) {
    /** Single date argument. */
    testSingleArg(Value(Date_t::fromMillisSinceEpoch(12345)),
                  Value(Date_t::fromMillisSinceEpoch(12345)));
}

TEST(ExpressionAddTest, Null) {
    /** Single null argument. */
    testSingleArg(Value(BSONNULL), Value(BSONNULL));
}

TEST(ExpressionAddTest, Undefined) {
    /** Single undefined argument. */
    testSingleArg(Value(BSONUndefined), Value(BSONNULL));
}

TEST(ExpressionAddTest, IntInt) {
    /** Add two ints. */
    testDoubleArg(Value(1), Value(5), Value(6));
}

TEST(ExpressionAddTest, IntIntNoOverflow) {
    /** Adding two large ints produces a long, not an overflowed int. */
    testDoubleArg(
        Value(std::numeric_limits<int>::max()),
        Value(std::numeric_limits<int>::max()),
        Value(((long long)(std::numeric_limits<int>::max()) + std::numeric_limits<int>::max())));
}

TEST(ExpressionAddTest, IntLong) {
    /** Adding an int and a long produces a long. */
    testDoubleArg(Value(1), Value(9LL), Value(10LL));
}

TEST(ExpressionAddTest, IntLongOverflowToDouble) {
    /** Adding an int and a long produces a double. */
    // When the result cannot be represented in a NumberLong, a NumberDouble is returned.
    const auto im = std::numeric_limits<int>::max();
    const auto llm = std::numeric_limits<long long>::max();
    double result = static_cast<double>(im) + static_cast<double>(llm);
    testDoubleArg(Value(std::numeric_limits<int>::max()),
                  Value(std::numeric_limits<long long>::max()),
                  Value(result));
}

TEST(ExpressionAddTest, IntDouble) {
    /** Adding an int and a double produces a double. */
    testDoubleArg(Value(9), Value(1.1), Value(10.1));
}

TEST(ExpressionAddTest, IntDate) {
    /** Adding an int and a Date produces a Date. */
    testDoubleArg(Value(6),
                  Value(Date_t::fromMillisSinceEpoch(123450)),
                  Value(Date_t::fromMillisSinceEpoch(123456)));
}

TEST(ExpressionAddTest, LongDouble) {
    /** Adding a long and a double produces a double. */
    testDoubleArg(Value(9LL), Value(1.1), Value(10.1));
}

TEST(ExpressionAddTest, LongDoubleNoOverflow) {
    /** Adding a long and a double does not overflow. */
    testDoubleArg(Value(std::numeric_limits<long long>::max()),
                  Value(double(std::numeric_limits<long long>::max())),
                  Value(static_cast<double>(std::numeric_limits<long long>::max()) +
                        static_cast<double>(std::numeric_limits<long long>::max())));
}

TEST(ExpressionAddTest, IntNull) {
    /** Adding an int and null. */
    testDoubleArg(Value(1), Value(BSONNULL), Value(BSONNULL));
}

TEST(ExpressionAddTest, LongUndefined) {
    /** Adding a long and undefined. */
    testDoubleArg(Value(5LL), Value(BSONUndefined), Value(BSONNULL));
}

TEST(ExpressionAddTest, Integers) {
    assertExpectedResults("$add",
                          {
                              // Empty case.
                              {{}, 0},
                              // Singleton case.
                              {{1}, 1},
                              // Integer addition.
                              {{1, 2, 3}, 6},
                              // Adding negative numbers
                              {{6, -3, 2}, 5},
                              // Getting a negative result
                              {{-6, -3, 2}, -7},
                              // Min/max ints are not promoted to longs.
                              {{INT_MAX}, INT_MAX},
                              {{INT_MAX, -1}, Value(INT_MAX - 1)},
                              {{INT_MIN}, INT_MIN},
                              {{INT_MIN, 1}, Value(INT_MIN + 1)},
                              // Integer overflow is promoted to a long.
                              {{INT_MAX, 1}, Value((long long)INT_MAX + 1LL)},
                              {{INT_MIN, -1}, Value((long long)INT_MIN - 1LL)},
                          });
}


TEST(ExpressionAddTest, Longs) {
    assertExpectedResults(
        "$add",
        {
            // Singleton case.
            {{1LL}, 1LL},
            // Long addition.
            {{1LL, 2LL, 3LL}, 6LL},
            // Adding negative numbers
            {{6LL, -3LL, 2LL}, 5LL},
            // Getting a negative result
            {{-6LL, -3LL, 2LL}, -7LL},
            // Confirm that NumberLong is wider than NumberInt, and the output
            // will be a long if any operand is a long.
            {{1LL, 2, 3LL}, 6LL},
            {{1LL, 2, 3}, 6LL},
            {{1, 2, 3LL}, 6LL},
            {{1, 2LL, 3LL}, 6LL},
            {{6, -3LL, 2}, 5LL},
            {{-6LL, -3, 2}, -7LL},
            // Min/max longs are not promoted to double.
            {{LLONG_MAX}, LLONG_MAX},
            {{LLONG_MAX, -1LL}, Value(LLONG_MAX - 1LL)},
            {{LLONG_MIN}, LLONG_MIN},
            {{LLONG_MIN, 1LL}, Value(LLONG_MIN + 1LL)},
            // Long overflow is promoted to a double.
            {{LLONG_MAX, 1LL}, Value((double)LLONG_MAX + 1.0)},
            // The result is "incorrect" here due to floating-point rounding errors.
            {{LLONG_MIN, -1LL}, Value((double)LLONG_MIN)},
        });
}

TEST(ExpressionAddTest, Doubles) {
    assertExpectedResults("$add",
                          {
                              // Singleton case.
                              {{1.0}, 1.0},
                              // Double addition.
                              {{1.0, 2.0, 3.0}, 6.0},
                              // Adding negative numbers
                              {{6.0, -3.0, 2.0}, 5.0},
                              // Getting a negative result
                              {{-6.0, -3.0, 2.0}, -7.0},
                              // Confirm that doubles are wider than ints and longs, and the output
                              // will be a double if any operand is a double.
                              {{1, 2, 3.0}, 6.0},
                              {{1LL, 2LL, 3.0}, 6.0},
                              {{3.0, 2, 1LL}, 6.0},
                              {{3, 2.0, 1LL}, 6.0},
                              {{-3, 2.0, 1LL}, 0.0},
                              {{-6LL, 2LL, 3.0}, -1.0},
                              {{-6.0, 2LL, 3.0}, -1.0},
                              // Confirm floating point arithmetic has rounding errors.
                              {{0.1, 0.2}, 0.30000000000000004},
                          });
}

TEST(ExpressionAddTest, Decimals) {
    assertExpectedResults(
        "$add",
        {
            // Singleton case.
            {{Decimal128(1)}, Decimal128(1)},
            // Decimal addition.
            {{Decimal128(1.0), Decimal128(2.0), Decimal128(3.0)}, Decimal128(6.0)},
            {{Decimal128(-6.0), Decimal128(2.0), Decimal128(3.0)}, Decimal128(-1.0)},
            // Confirm that decimals are wider than all other types, and the output
            // will be a double if any operand is a double.
            {{Decimal128(1), 2LL, 3}, Decimal128(6.0)},
            {{Decimal128(3), 2.0, 1LL}, Decimal128(6.0)},
            {{Decimal128(3), 2, 1.0}, Decimal128(6.0)},
            {{1, 2, Decimal128(3.0)}, Decimal128(6.0)},
            {{1LL, Decimal128(2.0), 3.0}, Decimal128(6.0)},
            {{1.0, 2.0, Decimal128(3.0)}, Decimal128(6.0)},
            {{1, Decimal128(2.0), 3.0}, Decimal128(6.0)},
            {{1LL, Decimal128(2.0), 3.0, 2}, Decimal128(8.0)},
            {{1LL, Decimal128(2.0), 3, 2.0}, Decimal128(8.0)},
            {{1, Decimal128(2.0), 3LL, 2.0}, Decimal128(8.0)},
            {{3.0, Decimal128(0.0), 2, 1LL}, Decimal128(6.0)},
            {{1, 3LL, 2.0, Decimal128(2.0)}, Decimal128(8.0)},
            {{3.0, 2, 1LL, Decimal128(0.0)}, Decimal128(6.0)},
            {{Decimal128(-6.0), 2.0, 3LL}, Decimal128(-1.0)},
        });
}

TEST(ExpressionAddTest, DatesNonDecimal) {
    assertExpectedResults(
        "$add",
        {
            {{1, 2, 3, Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(106)},
            {{1LL, 2LL, 3LL, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(106)},
            {{1.0, 2.0, 3.0, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(106)},
            {{1.0, 2.0, Value(Date_t::fromMillisSinceEpoch(100)), 3.0},
             Date_t::fromMillisSinceEpoch(106)},
            {{1.0, 2.2, 3.5, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1, 2.2, 3.5, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1, Date_t::fromMillisSinceEpoch(100), 2.2, 3.5}, Date_t::fromMillisSinceEpoch(107)},
            {{Date_t::fromMillisSinceEpoch(100), 1, 2.2, 3.5}, Date_t::fromMillisSinceEpoch(107)},
            {{-6, Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(94)},
            {{-200, Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(-100)},
            {{1, 2, 3, Date_t::fromMillisSinceEpoch(-100)}, Date_t::fromMillisSinceEpoch(-94)},
        });
}

TEST(ExpressionAddTest, DatesDecimal) {
    assertExpectedResults(
        "$add",
        {
            {{1, Decimal128(2), 3, Date_t::fromMillisSinceEpoch(100)},
             Date_t::fromMillisSinceEpoch(106)},
            {{1LL, 2LL, Decimal128(3LL), Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(106)},
            {{1, Decimal128(2.2), 3.5, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1, Decimal128(2.2), Decimal128(3.5), Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1.0, Decimal128(2.2), Decimal128(3.5), Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{Decimal128(-6), Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(94)},
            {{Decimal128(-200), Date_t::fromMillisSinceEpoch(100)},
             Date_t::fromMillisSinceEpoch(-100)},
            {{1, Decimal128(2), 3, Date_t::fromMillisSinceEpoch(-100)},
             Date_t::fromMillisSinceEpoch(-94)},
        });
}

TEST(ExpressionAddTest, Assertions) {
    // Date addition must fit in a NumberLong from a double.
    ASSERT_THROWS_CODE(
        evaluateExpression("$add", {Date_t::fromMillisSinceEpoch(100), (double)LLONG_MAX}),
        AssertionException,
        ErrorCodes::Overflow);

    // Only one date allowed in an $add expression.
    ASSERT_THROWS_CODE(
        evaluateExpression(
            "$add", {Date_t::fromMillisSinceEpoch(100), 1, Date_t::fromMillisSinceEpoch(100)}),
        AssertionException,
        16612);

    // Only numeric types are allowed in a $add.
    ASSERT_THROWS_CODE(evaluateExpression("$add", {1, 2, "not numeric!"_sd, 3}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(ExpressionAddTest, VerifyNoDoubleDoubleSummation) {
    // Confirm that we're not using DoubleDoubleSummation for $add expression with a set of double
    // values from mongo/util/summation_test.cpp.
    std::vector<ImplicitValue> doubleValues = {
        1.4831356930199802e-05,  -3.121724665346865,     3041897608700.073,
        1001318343149.7166,      -1714.6229586696593,    1731390114894580.8,
        6.256645803154374e-08,   -107144114533844.25,    -0.08839485091750919,
        -265119153.02185738,     -0.02450615965231944,   0.0002684331017079073,
        32079040427.68358,       -0.04733295911845742,   0.061381859083076085,
        -25329.59126796951,      -0.0009567520620034965, -1553879364344.9932,
        -2.1101077525869814e-08, -298421079729.5547,     0.03182394834273594,
        22.201944843278916,      -33.35667991109125,     11496013.960449915,
        -40652595.33210472,      3.8496066090328163,     2.5074042398147304e-08,
        -0.02208724071782122,    -134211.37290639878,    0.17640433666616578,
        4.463787499171126,       9.959669945399718,      129265976.35224283,
        1.5865526187526546e-07,  -4746011.710555799,     -712048598925.0789,
        582214206210.4034,       0.025236204812875362,   530078170.91147506,
        -14.865307666195053,     1.6727994895185032e-05, -113386276.03121366,
        -6.135827207137054,      10644945799901.145,     -100848907797.1582,
        2.2404406961625282e-08,  1.315662618424494e-09,  -0.832190208349044,
        -9.779323414999364,      -546522170658.2997};
    double straightSum = 0.0;
    DoubleDoubleSummation compensatedSum;
    for (const auto& x : doubleValues) {
        compensatedSum.addDouble(x.getDouble());
        straightSum += x.getDouble();
    }
    ASSERT_NE(straightSum, compensatedSum.getDouble());

    Value result = evaluateExpression("$add", doubleValues);
    ASSERT_VALUE_EQ(result, Value(straightSum));
    ASSERT_VALUE_NE(result, Value(compensatedSum.getDouble()));
}

/* ------------------------ Pow -------------------- */

TEST(ExpressionPowTest, LargeExponentValuesWithBaseOfZero) {
    assertExpectedResults(
        "$pow",
        {
            {{Value(0), Value(0)}, Value(1)},
            {{Value(0LL), Value(0LL)}, Value(1LL)},

            {{Value(0), Value(10)}, Value(0)},
            {{Value(0), Value(10000)}, Value(0)},

            {{Value(0LL), Value(10)}, Value(0LL)},

            // $pow may sometimes use a loop to compute a^b, so it's important to check
            // that the loop doesn't hang if a large exponent is provided.
            {{Value(0LL), Value(std::numeric_limits<long long>::max())}, Value(0LL)},
        });
}

TEST(ExpressionPowTest, ThrowsWhenBaseZeroAndExpNegative) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto expr =
        Expression::parseExpression(&expCtx, BSON("$pow" << BSON_ARRAY(0 << -5)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);

    const auto exprWithLong =
        Expression::parseExpression(&expCtx, BSON("$pow" << BSON_ARRAY(0LL << -5LL)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionPowTest, LargeExponentValuesWithBaseOfOne) {
    assertExpectedResults(
        "$pow",
        {
            {{Value(1), Value(10)}, Value(1)},
            {{Value(1), Value(10LL)}, Value(1LL)},
            {{Value(1), Value(10000LL)}, Value(1LL)},

            {{Value(1LL), Value(10LL)}, Value(1LL)},

            // $pow may sometimes use a loop to compute a^b, so it's important to check
            // that the loop doesn't hang if a large exponent is provided.
            {{Value(1LL), Value(std::numeric_limits<long long>::max())}, Value(1LL)},
            {{Value(1LL), Value(std::numeric_limits<long long>::min())}, Value(1LL)},
        });
}

TEST(ExpressionPowTest, LargeExponentValuesWithBaseOfNegativeOne) {
    assertExpectedResults("$pow",
                          {
                              {{Value(-1), Value(-1)}, Value(-1)},
                              {{Value(-1), Value(-2)}, Value(1)},
                              {{Value(-1), Value(-3)}, Value(-1)},

                              {{Value(-1LL), Value(0LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-1LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-2LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-3LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-4LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-5LL)}, Value(-1LL)},

                              {{Value(-1LL), Value(-61LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(61LL)}, Value(-1LL)},

                              {{Value(-1LL), Value(-62LL)}, Value(1LL)},
                              {{Value(-1LL), Value(62LL)}, Value(1LL)},

                              {{Value(-1LL), Value(-101LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-102LL)}, Value(1LL)},

                              // Use a value large enough that will make the test hang for a
                              // considerable amount of time if a loop is used to compute the
                              // answer.
                              {{Value(-1LL), Value(63234673905128LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-63234673905128LL)}, Value(1LL)},

                              {{Value(-1LL), Value(63234673905127LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-63234673905127LL)}, Value(-1LL)},
                          });
}

TEST(ExpressionPowTest, LargeBaseSmallPositiveExponent) {
    assertExpectedResults("$pow",
                          {
                              {{Value(4294967296LL), Value(1LL)}, Value(4294967296LL)},
                              {{Value(4294967296LL), Value(0)}, Value(1LL)},
                          });
}

namespace is_number {

TEST(ExpressionIsNumberTest, WithMinKeyValue) {
    assertExpectedResults("$isNumber", {{{Value(MINKEY)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithDoubleValue) {
    assertExpectedResults("$isNumber", {{{Value(1.0)}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithStringValue) {
    assertExpectedResults("$isNumber", {{{Value("stringValue"_sd)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithNumericStringValue) {
    assertExpectedResults("$isNumber", {{{Value("5"_sd)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithObjectValue) {
    BSONObj objectVal = fromjson("{a: {$literal: 1}}");
    assertExpectedResults("$isNumber", {{{Value(objectVal)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithArrayValue) {
    assertExpectedResults("$isNumber", {{{Value(BSON_ARRAY(1 << 2))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithBinDataValue) {
    BSONBinData binDataVal = BSONBinData("", 0, BinDataGeneral);
    assertExpectedResults("$isNumber", {{{Value(binDataVal)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithUndefinedValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONUndefined)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithOIDValue) {
    assertExpectedResults("$isNumber", {{{Value(OID())}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithBoolValue) {
    assertExpectedResults("$isNumber", {{{Value(true)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithDateValue) {
    Date_t dateVal = BSON("" << DATENOW).firstElement().Date();
    assertExpectedResults("$isNumber", {{{Value(dateVal)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithNullValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONNULL)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithRegexValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONRegEx("a.b"))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithSymbolValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONSymbol("a"))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithDBRefValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONDBRef("", OID()))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithCodeWScopeValue) {
    assertExpectedResults("$isNumber",
                          {{{Value(BSONCodeWScope("var x = 3", BSONObj()))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithCodeValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONCode("var x = 3"))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithIntValue) {
    assertExpectedResults("$isNumber", {{{Value(1)}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithDecimalValue) {
    assertExpectedResults("$isNumber", {{{Value(Decimal128(0.3))}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithLongValue) {
    assertExpectedResults("$isNumber", {{{Value(1LL)}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithTimestampValue) {
    assertExpectedResults("$isNumber", {{{Value(Timestamp(0, 0))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithMaxKeyValue) {
    assertExpectedResults("$isNumber", {{{Value(MAXKEY)}, Value(false)}});
}

}  // namespace is_number

/* ------------------------ Rand -------------------- */

void assertRandomProperties(const std::function<double(void)>& fn) {
    double sum = 0.0;
    constexpr int N = 1000000;

    for (int i = 0; i < N; i++) {
        const double v = fn();
        ASSERT_LTE(0.0, v);
        ASSERT_GTE(1.0, v);
        sum += v;
    }

    const double avg = sum / N;
    // For continuous uniform distribution [0.0, 1.0] the variance is 1/12.
    // Test certainty within 10 standard deviations.
    const double err = 10.0 / sqrt(12.0 * N);
    ASSERT_LT(0.5 - err, avg);
    ASSERT_GT(0.5 + err, avg);
}

TEST(ExpressionRandom, Basic) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // We generate a new random value on every call to evaluate().
    intrusive_ptr<Expression> expression =
        Expression::parseExpression(&expCtx, fromjson("{ $rand: {} }"), vps);

    const std::string& serialized = expression->serialize().getDocument().toString();
    ASSERT_EQ("{$rand: {}}", serialized);

    const auto randFn = [&expression, &expCtx]() -> double {
        return expression->evaluate({}, &expCtx.variables).getDouble();
    };
    assertRandomProperties(randFn);
}

/* ------------------------ Subtract -------------------- */

TEST(ExpressionSubtractTest, OverflowLong) {
    const auto maxLong = std::numeric_limits<long long int>::max();
    const auto minLong = std::numeric_limits<long long int>::min();
    auto expCtx = ExpressionContextForTest{};

    // The following subtractions should not fit into a long long data type.
    BSONObj obj = BSON("$subtract" << BSON_ARRAY(maxLong << minLong));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_EQ(result.getDouble(), static_cast<double>(maxLong) - minLong);

    obj = BSON("$subtract" << BSON_ARRAY(minLong << maxLong));
    expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_EQ(result.getDouble(), static_cast<double>(minLong) - static_cast<double>(maxLong));

    // minLong = -1 - maxLong. The below subtraction should fit into long long data type.
    obj = BSON("$subtract" << BSON_ARRAY(-1 << maxLong));
    expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::numberLong);
    ASSERT_EQ(result.getLong(), -1LL - maxLong);

    // The minLong's negation does not fit into long long, hence it should be converted to double
    // data type.
    obj = BSON("$subtract" << BSON_ARRAY(0 << minLong));
    expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_EQ(result.getDouble(), static_cast<double>(minLong) * -1);
}

/* ------------------------ Mod -------------------- */

TEST(ExpressionModTest, ModWithDoubleDoubleTypeButIntegralValues) {
    // Test that $mod with args of type double/double returns a value with type double,
    // _even_ if the values happen to be an integral number (could be converted to an int with no
    // rounding error).
    using namespace mongo::literals;
    assertExpectedResults(
        "$mod", {{{Value(1.0), Value(2.0)}, Value(1.0)}, {{Value(3.0), Value(2.0)}, Value(1.0)}});
}

TEST(ExpressionModTest, ModWithDoubleLongTypeButIntegralValues) {
    // As above, for double/long.
    using namespace mongo::literals;
    assertExpectedResults("$mod",
                          {{{Value(3ll), Value(2.0)}, Value(1.0)},
                           {{Value(3.0), Value(2.0)}, Value(1.0)},
                           {{Value(3.0), Value(2ll)}, Value(1.0)}});
}

TEST(ExpressionModTest, ModWithDoubleIntTypeButIntegralValues) {
    // As above, for double/int.
    using namespace mongo::literals;
    assertExpectedResults("$mod",
                          {{{Value(3), Value(2.0)}, Value(1.0)},
                           {{Value(3.0), Value(2.0)}, Value(1.0)},
                           {{Value(3.0), Value(2)}, Value(1.0)}});
}

namespace convert {
/**
 * Generates a random double with a variable number of decimal places between 1 and 15.
 */
double randomDouble() {
    // Create a random number generator engine.
    std::random_device rd;
    std::mt19937 gen(rd());

    // Create a distribution and generate a double between -1 and 1.
    std::uniform_real_distribution<double> dis(-1.0, 1.0);
    double randomValue = dis(gen);

    std::uniform_int_distribution<int> multiplier(0, 15);
    int shift = multiplier(gen);
    double factor = std::pow(10.0, shift);

    // Multiply the random number by the factor to set the decimal places
    double result = randomValue * factor;
    return result;
}

/**
 * Verifies that a double can correctly convert to a string and round-trip back to the original
 * double.
 */
void verifyStringDoubleConvertRoundtripsCorrectly(double doubleToConvert) {
    Value doubleConvertedToString = evaluateExpression("$toString", {doubleToConvert});
    ASSERT_EQ(doubleConvertedToString.getType(), BSONType::string);

    Value stringConvertedToDouble = evaluateExpression("$toDouble", {doubleConvertedToString});
    ASSERT_EQ(stringConvertedToDouble.getType(), BSONType::numberDouble);

    // Verify the conversion round-trips correctly.
    ASSERT_VALUE_EQ(stringConvertedToDouble, Value(doubleToConvert));
}

/**
 * Test case for round-trip conversion of random double using $convert.
 *
 * Generates 1000 random doubles and verifies they can be correctly converted to string values and
 * back to double.
 */
TEST(ExpressionConvert, StringToDouble) {
    for (int i = 0; i < 1000; ++i) {
        verifyStringDoubleConvertRoundtripsCorrectly(randomDouble());
    }
}

}  // namespace convert

}  // namespace expression_evaluation_test
}  // namespace mongo
