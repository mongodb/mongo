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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/expression_trigonometric.h"

#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace expression_tests {

using boost::intrusive_ptr;
using namespace mongo;

// assertApproxEq is a helper function for asserting approximate results.
static void assertApproxEq(const Value& evaluated, const Value& expected) {
    ASSERT_EQ(evaluated.getType(), expected.getType());
    if (expected.nullish()) {
        ASSERT_VALUE_EQ(expected, evaluated);
    } else {
        ASSERT_VALUE_LT(
            Value(evaluated.coerceToDecimal().subtract(expected.coerceToDecimal()).toAbs()),
            Value(Decimal128(".000001")));
    }
}

// A testing class for testing approximately equal results for one argument numeric expressions.
static void assertEvaluates(const std::string& expressionName, Value input, Value output) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto obj = BSON(expressionName << BSON_ARRAY(input));
    auto vps = expCtx->variablesParseState;
    auto expression = Expression::parseExpression(expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx->variables);
    ASSERT_EQUALS(result.getType(), output.getType());
    assertApproxEq(result, output);
}


// A testing class for testing approximately equal results for two argument numeric expressions.
static void assertEvaluates(const std::string& expressionName,
                            Value input1,
                            Value input2,
                            Value output) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto obj = BSON(expressionName << BSON_ARRAY(input1 << input2));
    auto vps = expCtx->variablesParseState;
    auto expression = Expression::parseExpression(expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx->variables);
    ASSERT_EQUALS(result.getType(), output.getType());
    assertApproxEq(result, output);
}

/* ------------------------- ExpressionArcSine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of asin, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionArcSineTest, IntArg) {
    assertEvaluates("$asin", Value(0), Value(0.0));
    assertEvaluates("$asin", Value(1), Value(1.57079632679));
}

TEST(ExpressionArcSineTest, LongArg) {
    assertEvaluates("$asin", Value(0LL), Value(0.0));
    assertEvaluates("$asin", Value(1LL), Value(1.57079632679));
}

TEST(ExpressionArcSineTest, DoubleArg) {
    assertEvaluates("$asin", Value(0.0), Value(0.0));
    assertEvaluates("$asin", Value(0.1), Value(0.100167421162));
    assertEvaluates("$asin", Value(0.2), Value(0.20135792079));
    assertEvaluates("$asin", Value(0.3), Value(0.304692654015));
    assertEvaluates("$asin", Value(0.4), Value(0.411516846067));
    assertEvaluates("$asin", Value(0.5), Value(0.523598775598));
    assertEvaluates("$asin", Value(0.6), Value(0.643501108793));
    assertEvaluates("$asin", Value(0.7), Value(0.775397496611));
    assertEvaluates("$asin", Value(0.8), Value(0.927295218002));
    assertEvaluates("$asin", Value(0.9), Value(1.119769515));
    assertEvaluates("$asin", Value(1.0), Value(1.57079632679));
}

TEST(ExpressionArcSineTest, DecimalArg) {
    assertEvaluates("$asin", Value(Decimal128("0.0")), Value(Decimal128("0.0")));
    assertEvaluates("$asin", Value(Decimal128("0.1")), Value(Decimal128("0.100167421162")));
    assertEvaluates("$asin", Value(Decimal128("0.2")), Value(Decimal128("0.20135792079")));
    assertEvaluates("$asin", Value(Decimal128("0.3")), Value(Decimal128("0.304692654015")));
    assertEvaluates("$asin", Value(Decimal128("0.4")), Value(Decimal128("0.411516846067")));
    assertEvaluates("$asin", Value(Decimal128("0.5")), Value(Decimal128("0.523598775598")));
    assertEvaluates("$asin", Value(Decimal128("0.6")), Value(Decimal128("0.643501108793")));
    assertEvaluates("$asin", Value(Decimal128("0.7")), Value(Decimal128("0.775397496611")));
    assertEvaluates("$asin", Value(Decimal128("0.8")), Value(Decimal128("0.927295218002")));
    assertEvaluates("$asin", Value(Decimal128("0.9")), Value(Decimal128("1.119769515")));
    assertEvaluates("$asin", Value(Decimal128("1.0")), Value(Decimal128("1.57079632679")));
}

TEST(ExpressionArcSineTest, NullArg) {
    assertEvaluates("$asin", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionArcCosine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of acos, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionArcCosineTest, IntArg) {
    assertEvaluates("$acos", Value(0), Value(1.57079632679));
    assertEvaluates("$acos", Value(1), Value(0.0));
}

TEST(ExpressionArcCosineTest, LongArg) {
    assertEvaluates("$acos", Value(0LL), Value(1.57079632679));
    assertEvaluates("$acos", Value(1LL), Value(0.0));
}

TEST(ExpressionArcCosineTest, DoubleArg) {
    assertEvaluates("$acos", Value(0.0), Value(1.57079632679));
    assertEvaluates("$acos", Value(0.1), Value(1.47062890563));
    assertEvaluates("$acos", Value(0.2), Value(1.369438406));
    assertEvaluates("$acos", Value(0.3), Value(1.26610367278));
    assertEvaluates("$acos", Value(0.4), Value(1.15927948073));
    assertEvaluates("$acos", Value(0.5), Value(1.0471975512));
    assertEvaluates("$acos", Value(0.6), Value(0.927295218002));
    assertEvaluates("$acos", Value(0.7), Value(0.795398830184));
    assertEvaluates("$acos", Value(0.8), Value(0.643501108793));
    assertEvaluates("$acos", Value(0.9), Value(0.451026811796));
    assertEvaluates("$acos", Value(1.0), Value(0.0));
}

TEST(ExpressionArcCosineTest, DecimalArg) {
    assertEvaluates("$acos", Value(Decimal128("0.0")), Value(Decimal128("1.57079632679")));
    assertEvaluates("$acos", Value(Decimal128("0.1")), Value(Decimal128("1.47062890563")));
    assertEvaluates("$acos", Value(Decimal128("0.2")), Value(Decimal128("1.369438406")));
    assertEvaluates("$acos", Value(Decimal128("0.3")), Value(Decimal128("1.26610367278")));
    assertEvaluates("$acos", Value(Decimal128("0.4")), Value(Decimal128("1.15927948073")));
    assertEvaluates("$acos", Value(Decimal128("0.5")), Value(Decimal128("1.0471975512")));
    assertEvaluates("$acos", Value(Decimal128("0.6")), Value(Decimal128("0.927295218002")));
    assertEvaluates("$acos", Value(Decimal128("0.7")), Value(Decimal128("0.795398830184")));
    assertEvaluates("$acos", Value(Decimal128("0.8")), Value(Decimal128("0.643501108793")));
    assertEvaluates("$acos", Value(Decimal128("0.9")), Value(Decimal128("0.451026811796")));
    assertEvaluates("$acos", Value(Decimal128("1.0")), Value(Decimal128("0.0")));
}

TEST(ExpressionArcCosineTest, NullArg) {
    assertEvaluates("$acos", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionArcTangent -------------------------- */
/**
 * Test values were generated using the 64 bit std version of atan, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionArcTangentTest, IntArg) {
    assertEvaluates("$atan", Value(-1), Value(-0.785398163397));
    assertEvaluates("$atan", Value(0), Value(0.0));
    assertEvaluates("$atan", Value(1), Value(0.785398163397));
}

TEST(ExpressionArcTangentTest, LongArg) {
    assertEvaluates("$atan", Value(-1LL), Value(-0.785398163397));
    assertEvaluates("$atan", Value(0LL), Value(0.0));
    assertEvaluates("$atan", Value(1LL), Value(0.785398163397));
}

TEST(ExpressionArcTangentTest, DoubleArg) {
    assertEvaluates("$atan", Value(-1.5), Value(-0.982793723247));
    assertEvaluates("$atan", Value(-1.0471975512), Value(-0.80844879263));
    assertEvaluates("$atan", Value(-0.785398163397), Value(-0.665773750028));
    assertEvaluates("$atan", Value(0), Value(0.0));
    assertEvaluates("$atan", Value(0.785398163397), Value(0.665773750028));
    assertEvaluates("$atan", Value(1.0471975512), Value(0.80844879263));
    assertEvaluates("$atan", Value(1.5), Value(0.982793723247));
}

TEST(ExpressionArcTangentTest, DecimalArg) {
    assertEvaluates("$atan", Value(Decimal128("-1.5")), Value(Decimal128("-0.982793723247")));
    assertEvaluates(
        "$atan", Value(Decimal128("-1.0471975512")), Value(Decimal128("-0.80844879263")));
    assertEvaluates(
        "$atan", Value(Decimal128("-0.785398163397")), Value(Decimal128("-0.665773750028")));
    assertEvaluates("$atan", Value(Decimal128("0")), Value(Decimal128("0.0")));
    assertEvaluates(
        "$atan", Value(Decimal128("0.785398163397")), Value(Decimal128("0.665773750028")));
    assertEvaluates("$atan", Value(Decimal128("1.0471975512")), Value(Decimal128("0.80844879263")));
    assertEvaluates("$atan", Value(Decimal128("1.5")), Value(Decimal128("0.982793723247")));
}

TEST(ExpressionArcTangentTest, NullArg) {
    assertEvaluates("$atan", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionArcTangent2 -------------------------- */
/**
 * Test values were generated using the 64 bit std version of atan2, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionArcTangent2Test, TwoIntArgs) {
    assertEvaluates("$atan2", Value(1), Value(0), Value(1.57079632679));
    assertEvaluates("$atan2", Value(0), Value(1), Value(0.0));
    assertEvaluates("$atan2", Value(-1), Value(0), Value(-1.57079632679));
    assertEvaluates("$atan2", Value(0), Value(-1), Value(3.14159265359));
}

TEST(ExpressionArcTangent2Test, TwoLongArg) {
    assertEvaluates("$atan2", Value(1LL), Value(0LL), Value(1.57079632679));
    assertEvaluates("$atan2", Value(0LL), Value(1LL), Value(0.0));
    assertEvaluates("$atan2", Value(-1LL), Value(0LL), Value(-1.57079632679));
    assertEvaluates("$atan2", Value(0LL), Value(-1LL), Value(3.14159265359));
}

TEST(ExpressionArcTangent2Test, LongIntArg) {
    assertEvaluates("$atan2", Value(1LL), Value(0), Value(1.57079632679));
    assertEvaluates("$atan2", Value(0LL), Value(1), Value(0.0));
    assertEvaluates("$atan2", Value(-1LL), Value(0), Value(-1.57079632679));
    assertEvaluates("$atan2", Value(0LL), Value(-1), Value(3.14159265359));
}

TEST(ExpressionArcTangent2Test, IntLongArg) {
    assertEvaluates("$atan2", Value(1), Value(0LL), Value(1.57079632679));
    assertEvaluates("$atan2", Value(0), Value(1LL), Value(0.0));
    assertEvaluates("$atan2", Value(-1), Value(0LL), Value(-1.57079632679));
    assertEvaluates("$atan2", Value(0), Value(-1LL), Value(3.14159265359));
}

TEST(ExpressionArcTangent2Test, TwoDoubleArg) {
    assertEvaluates("$atan2", Value(1.0), Value(0.0), Value(1.57079632679));
    assertEvaluates("$atan2", Value(0.866025403784), Value(0.5), Value(1.0471975512));
    assertEvaluates("$atan2", Value(0.707106781187), Value(0.707106781187), Value(0.785398163397));
    assertEvaluates("$atan2", Value(0.5), Value(0.866025403784), Value(0.523598775598));
    assertEvaluates("$atan2", Value(6.12323399574e-17), Value(1.0), Value(6.12323399574e-17));
    assertEvaluates("$atan2", Value(-0.5), Value(0.866025403784), Value(-0.523598775598));
    assertEvaluates(
        "$atan2", Value(-0.707106781187), Value(0.707106781187), Value(-0.785398163397));
    assertEvaluates("$atan2", Value(-0.866025403784), Value(0.5), Value(-1.0471975512));
    assertEvaluates("$atan2", Value(-1.0), Value(1.22464679915e-16), Value(-1.57079632679));
    assertEvaluates("$atan2", Value(-0.866025403784), Value(-0.5), Value(-2.09439510239));
    assertEvaluates(
        "$atan2", Value(-0.707106781187), Value(-0.707106781187), Value(-2.35619449019));
    assertEvaluates("$atan2", Value(-0.5), Value(-0.866025403784), Value(-2.61799387799));
    assertEvaluates("$atan2", Value(-1.83697019872e-16), Value(-1.0), Value(-3.14159265359));
    assertEvaluates("$atan2", Value(0.5), Value(-0.866025403784), Value(2.61799387799));
    assertEvaluates("$atan2", Value(0.707106781187), Value(-0.707106781187), Value(2.35619449019));
    assertEvaluates("$atan2", Value(0.866025403784), Value(-0.5), Value(2.09439510239));
    assertEvaluates("$atan2", Value(1.0), Value(-2.44929359829e-16), Value(1.57079632679));
}

TEST(ExpressionArcTangent2Test, TwoDecimalArg) {
    assertEvaluates("$atan2",
                    Value(Decimal128("1.0")),
                    Value(Decimal128("0.0")),
                    Value(Decimal128("1.57079632679")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.866025403784")),
                    Value(Decimal128("0.5")),
                    Value(Decimal128("1.0471975512")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.707106781187")),
                    Value(Decimal128("0.707106781187")),
                    Value(Decimal128("0.785398163397")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.5")),
                    Value(Decimal128("0.866025403784")),
                    Value(Decimal128("0.523598775598")));
    assertEvaluates("$atan2",
                    Value(Decimal128("6.12323399574e-17")),
                    Value(Decimal128("1.0")),
                    Value(Decimal128("6.12323399574e-17")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.5")),
                    Value(Decimal128("0.866025403784")),
                    Value(Decimal128("-0.523598775598")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.707106781187")),
                    Value(Decimal128("0.707106781187")),
                    Value(Decimal128("-0.785398163397")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.866025403784")),
                    Value(Decimal128("0.5")),
                    Value(Decimal128("-1.0471975512")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-1.0")),
                    Value(Decimal128("1.22464679915e-16")),
                    Value(Decimal128("-1.57079632679")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.866025403784")),
                    Value(Decimal128("-0.5")),
                    Value(Decimal128("-2.09439510239")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.707106781187")),
                    Value(Decimal128("-0.707106781187")),
                    Value(Decimal128("-2.35619449019")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.5")),
                    Value(Decimal128("-0.866025403784")),
                    Value(Decimal128("-2.61799387799")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-1.83697019872e-16")),
                    Value(Decimal128("-1.0")),
                    Value(Decimal128("-3.14159265359")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.5")),
                    Value(Decimal128("-0.866025403784")),
                    Value(Decimal128("2.61799387799")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.707106781187")),
                    Value(Decimal128("-0.707106781187")),
                    Value(Decimal128("2.35619449019")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.866025403784")),
                    Value(Decimal128("-0.5")),
                    Value(Decimal128("2.09439510239")));
    assertEvaluates("$atan2",
                    Value(Decimal128("1.0")),
                    Value(Decimal128("-2.44929359829e-16")),
                    Value(Decimal128("1.57079632679")));
}

TEST(ExpressionArcTangent2Test, DoubleDecimalArg) {
    assertEvaluates(
        "$atan2", Value(1.0), Value(Decimal128("0.0")), Value(Decimal128("1.57079632679")));
    assertEvaluates("$atan2",
                    Value(0.866025403784),
                    Value(Decimal128("0.5")),
                    Value(Decimal128("1.0471975512")));
    assertEvaluates("$atan2",
                    Value(0.707106781187),
                    Value(Decimal128("0.707106781187")),
                    Value(Decimal128("0.785398163397")));
    assertEvaluates("$atan2",
                    Value(0.5),
                    Value(Decimal128("0.866025403784")),
                    Value(Decimal128("0.523598775598")));
    assertEvaluates("$atan2",
                    Value(6.12323399574e-17),
                    Value(Decimal128("1.0")),
                    Value(Decimal128("6.12323399574e-17")));
    assertEvaluates("$atan2",
                    Value(-0.5),
                    Value(Decimal128("0.866025403784")),
                    Value(Decimal128("-0.523598775598")));
    assertEvaluates("$atan2",
                    Value(-0.707106781187),
                    Value(Decimal128("0.707106781187")),
                    Value(Decimal128("-0.785398163397")));
    assertEvaluates("$atan2",
                    Value(-0.866025403784),
                    Value(Decimal128("0.5")),
                    Value(Decimal128("-1.0471975512")));
    assertEvaluates("$atan2",
                    Value(-1.0),
                    Value(Decimal128("1.22464679915e-16")),
                    Value(Decimal128("-1.57079632679")));
    assertEvaluates("$atan2",
                    Value(-0.866025403784),
                    Value(Decimal128("-0.5")),
                    Value(Decimal128("-2.09439510239")));
    assertEvaluates("$atan2",
                    Value(-0.707106781187),
                    Value(Decimal128("-0.707106781187")),
                    Value(Decimal128("-2.35619449019")));
    assertEvaluates("$atan2",
                    Value(-0.5),
                    Value(Decimal128("-0.866025403784")),
                    Value(Decimal128("-2.61799387799")));
    assertEvaluates("$atan2",
                    Value(-1.83697019872e-16),
                    Value(Decimal128("-1.0")),
                    Value(Decimal128("-3.14159265359")));
    assertEvaluates("$atan2",
                    Value(0.5),
                    Value(Decimal128("-0.866025403784")),
                    Value(Decimal128("2.61799387799")));
    assertEvaluates("$atan2",
                    Value(0.707106781187),
                    Value(Decimal128("-0.707106781187")),
                    Value(Decimal128("2.35619449019")));
    assertEvaluates("$atan2",
                    Value(0.866025403784),
                    Value(Decimal128("-0.5")),
                    Value(Decimal128("2.09439510239")));
    assertEvaluates("$atan2",
                    Value(1.0),
                    Value(Decimal128("-2.44929359829e-16")),
                    Value(Decimal128("1.57079632679")));
}

TEST(ExpressionArcTangent2Test, DecimalDoubleArg) {
    assertEvaluates(
        "$atan2", Value(Decimal128("1.0")), Value(0.0), Value(Decimal128("1.57079632679")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.866025403784")),
                    Value(0.5),
                    Value(Decimal128("1.0471975512")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.707106781187")),
                    Value(0.707106781187),
                    Value(Decimal128("0.785398163397")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.5")),
                    Value(0.866025403784),
                    Value(Decimal128("0.523598775598")));
    assertEvaluates("$atan2",
                    Value(Decimal128("6.12323399574e-17")),
                    Value(1.0),
                    Value(Decimal128("6.12323399574e-17")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.5")),
                    Value(0.866025403784),
                    Value(Decimal128("-0.523598775598")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.707106781187")),
                    Value(0.707106781187),
                    Value(Decimal128("-0.785398163397")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.866025403784")),
                    Value(0.5),
                    Value(Decimal128("-1.0471975512")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-1.0")),
                    Value(1.22464679915e-16),
                    Value(Decimal128("-1.57079632679")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.866025403784")),
                    Value(-0.5),
                    Value(Decimal128("-2.09439510239")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.707106781187")),
                    Value(-0.707106781187),
                    Value(Decimal128("-2.35619449019")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-0.5")),
                    Value(-0.866025403784),
                    Value(Decimal128("-2.61799387799")));
    assertEvaluates("$atan2",
                    Value(Decimal128("-1.83697019872e-16")),
                    Value(-1.0),
                    Value(Decimal128("-3.14159265359")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.5")),
                    Value(-0.866025403784),
                    Value(Decimal128("2.61799387799")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.707106781187")),
                    Value(-0.707106781187),
                    Value(Decimal128("2.35619449019")));
    assertEvaluates("$atan2",
                    Value(Decimal128("0.866025403784")),
                    Value(-0.5),
                    Value(Decimal128("2.09439510239")));
    assertEvaluates("$atan2",
                    Value(Decimal128("1.0")),
                    Value(-2.44929359829e-16),
                    Value(Decimal128("1.57079632679")));
}

TEST(ExpressionArcTangent2Test, NullArg) {
    assertEvaluates("$atan2", Value(BSONNULL), Value(BSONNULL), Value(BSONNULL));
    assertEvaluates("$atan2", Value(1), Value(BSONNULL), Value(BSONNULL));
    assertEvaluates("$atan2", Value(BSONNULL), Value(1), Value(BSONNULL));
}

/* ------------------------- ExpressionCosine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of acos, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionCosineTest, IntArg) {
    assertEvaluates("$cos", Value(0), Value(1.0));
    assertEvaluates("$cos", Value(1), Value(0.540302305868));
    assertEvaluates("$cos", Value(2), Value(-0.416146836547));
    assertEvaluates("$cos", Value(3), Value(-0.9899924966));
    assertEvaluates("$cos", Value(4), Value(-0.653643620864));
    assertEvaluates("$cos", Value(5), Value(0.283662185463));
    assertEvaluates("$cos", Value(6), Value(0.96017028665));
}

TEST(ExpressionCosineTest, LongArg) {
    assertEvaluates("$cos", Value(0LL), Value(1.0));
    assertEvaluates("$cos", Value(1LL), Value(0.540302305868));
    assertEvaluates("$cos", Value(2LL), Value(-0.416146836547));
    assertEvaluates("$cos", Value(3LL), Value(-0.9899924966));
    assertEvaluates("$cos", Value(4LL), Value(-0.653643620864));
    assertEvaluates("$cos", Value(5LL), Value(0.283662185463));
    assertEvaluates("$cos", Value(6LL), Value(0.96017028665));
}

TEST(ExpressionCosineTest, DoubleArg) {
    assertEvaluates("$cos", Value(0.0), Value(1.0));
    assertEvaluates("$cos", Value(0.523598775598), Value(0.866025403784));
    assertEvaluates("$cos", Value(0.785398163397), Value(0.707106781187));
    assertEvaluates("$cos", Value(1.0471975512), Value(0.5));
    assertEvaluates("$cos", Value(1.57079632679), Value(6.12323399574e-17));
    assertEvaluates("$cos", Value(2.09439510239), Value(-0.5));
    assertEvaluates("$cos", Value(2.35619449019), Value(-0.707106781187));
    assertEvaluates("$cos", Value(2.61799387799), Value(-0.866025403784));
    assertEvaluates("$cos", Value(3.14159265359), Value(-1.0));
    assertEvaluates("$cos", Value(3.66519142919), Value(-0.866025403784));
    assertEvaluates("$cos", Value(3.92699081699), Value(-0.707106781187));
    assertEvaluates("$cos", Value(4.18879020479), Value(-0.5));
    assertEvaluates("$cos", Value(4.71238898038), Value(-1.83697019872e-16));
    assertEvaluates("$cos", Value(5.23598775598), Value(0.5));
    assertEvaluates("$cos", Value(5.49778714378), Value(0.707106781187));
    assertEvaluates("$cos", Value(5.75958653158), Value(0.866025403784));
    assertEvaluates("$cos", Value(6.28318530718), Value(1.0));
}

TEST(ExpressionCosineTest, DecimalArg) {
    assertEvaluates("$cos", Value(Decimal128("0.0")), Value(Decimal128("1.0")));
    assertEvaluates(
        "$cos", Value(Decimal128("0.523598775598")), Value(Decimal128("0.866025403784")));
    assertEvaluates(
        "$cos", Value(Decimal128("0.785398163397")), Value(Decimal128("0.707106781187")));
    assertEvaluates("$cos", Value(Decimal128("1.0471975512")), Value(Decimal128("0.5")));
    assertEvaluates(
        "$cos", Value(Decimal128("1.57079632679")), Value(Decimal128("6.12323399574e-17")));
    assertEvaluates("$cos", Value(Decimal128("2.09439510239")), Value(Decimal128("-0.5")));
    assertEvaluates(
        "$cos", Value(Decimal128("2.35619449019")), Value(Decimal128("-0.707106781187")));
    assertEvaluates(
        "$cos", Value(Decimal128("2.61799387799")), Value(Decimal128("-0.866025403784")));
    assertEvaluates("$cos", Value(Decimal128("3.14159265359")), Value(Decimal128("-1.0")));
    assertEvaluates(
        "$cos", Value(Decimal128("3.66519142919")), Value(Decimal128("-0.866025403784")));
    assertEvaluates(
        "$cos", Value(Decimal128("3.92699081699")), Value(Decimal128("-0.707106781187")));
    assertEvaluates("$cos", Value(Decimal128("4.18879020479")), Value(Decimal128("-0.5")));
    assertEvaluates(
        "$cos", Value(Decimal128("4.71238898038")), Value(Decimal128("-1.83697019872e-16")));
    assertEvaluates("$cos", Value(Decimal128("5.23598775598")), Value(Decimal128("0.5")));
    assertEvaluates(
        "$cos", Value(Decimal128("5.49778714378")), Value(Decimal128("0.707106781187")));
    assertEvaluates(
        "$cos", Value(Decimal128("5.75958653158")), Value(Decimal128("0.866025403784")));
    assertEvaluates("$cos", Value(Decimal128("6.28318530718")), Value(Decimal128("1.0")));
}

TEST(ExpressionCosineTest, NullArg) {
    assertEvaluates("$cos", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionHyperbolicCosine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of cosh, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionHyperbolicCosineTest, IntArg) {
    assertEvaluates("$cosh", Value(0), Value(1.0));
    assertEvaluates("$cosh", Value(1), Value(1.54308063482));
    assertEvaluates("$cosh", Value(2), Value(3.76219569108));
    assertEvaluates("$cosh", Value(3), Value(10.0676619958));
    assertEvaluates("$cosh", Value(4), Value(27.308232836));
    assertEvaluates("$cosh", Value(5), Value(74.2099485248));
    assertEvaluates("$cosh", Value(6), Value(201.715636122));
}

TEST(ExpressionHyperbolicCosineTest, LongArg) {
    assertEvaluates("$cosh", Value(0LL), Value(1.0));
    assertEvaluates("$cosh", Value(1LL), Value(1.54308063482));
    assertEvaluates("$cosh", Value(2LL), Value(3.76219569108));
    assertEvaluates("$cosh", Value(3LL), Value(10.0676619958));
    assertEvaluates("$cosh", Value(4LL), Value(27.308232836));
    assertEvaluates("$cosh", Value(5LL), Value(74.2099485248));
    assertEvaluates("$cosh", Value(6LL), Value(201.715636122));
}

TEST(ExpressionHyperbolicCosineTest, DoubleArg) {
    assertEvaluates("$cosh", Value(0.0), Value(1.0));
    assertEvaluates("$cosh", Value(0.523598775598), Value(1.14023832108));
    assertEvaluates("$cosh", Value(0.785398163397), Value(1.32460908925));
    assertEvaluates("$cosh", Value(1.0471975512), Value(1.6002868577));
    assertEvaluates("$cosh", Value(1.57079632679), Value(2.50917847866));
    assertEvaluates("$cosh", Value(2.09439510239), Value(4.12183605387));
    assertEvaluates("$cosh", Value(2.35619449019), Value(5.32275214952));
    assertEvaluates("$cosh", Value(2.61799387799), Value(6.89057236498));
    assertEvaluates("$cosh", Value(3.14159265359), Value(11.5919532755));
    assertEvaluates("$cosh", Value(3.66519142919), Value(19.5446063168));
    assertEvaluates("$cosh", Value(3.92699081699), Value(25.3868611924));
    assertEvaluates("$cosh", Value(4.18879020479), Value(32.97906491));
    assertEvaluates("$cosh", Value(4.71238898038), Value(55.6633808904));
    assertEvaluates("$cosh", Value(5.23598775598), Value(93.9599750339));
    assertEvaluates("$cosh", Value(5.49778714378), Value(122.07757934));
    assertEvaluates("$cosh", Value(5.75958653158), Value(158.610147472));
    assertEvaluates("$cosh", Value(6.28318530718), Value(267.746761484));
}

TEST(ExpressionHyperbolicCosineTest, DecimalArg) {
    assertEvaluates("$cosh", Value(Decimal128("0.0")), Value(Decimal128("1.0")));
    assertEvaluates(
        "$cosh", Value(Decimal128("0.523598775598")), Value(Decimal128("1.14023832108")));
    assertEvaluates(
        "$cosh", Value(Decimal128("0.785398163397")), Value(Decimal128("1.32460908925")));
    assertEvaluates("$cosh", Value(Decimal128("1.0471975512")), Value(Decimal128("1.6002868577")));
    assertEvaluates(
        "$cosh", Value(Decimal128("1.57079632679")), Value(Decimal128("2.50917847866")));
    assertEvaluates(
        "$cosh", Value(Decimal128("2.09439510239")), Value(Decimal128("4.12183605387")));
    assertEvaluates(
        "$cosh", Value(Decimal128("2.35619449019")), Value(Decimal128("5.32275214952")));
    assertEvaluates(
        "$cosh", Value(Decimal128("2.61799387799")), Value(Decimal128("6.89057236498")));
    assertEvaluates(
        "$cosh", Value(Decimal128("3.14159265359")), Value(Decimal128("11.5919532755")));
    assertEvaluates(
        "$cosh", Value(Decimal128("3.66519142919")), Value(Decimal128("19.5446063168")));
    assertEvaluates(
        "$cosh", Value(Decimal128("3.92699081699")), Value(Decimal128("25.3868611924")));
    assertEvaluates("$cosh", Value(Decimal128("4.18879020479")), Value(Decimal128("32.97906491")));
    assertEvaluates(
        "$cosh", Value(Decimal128("4.71238898038")), Value(Decimal128("55.6633808904")));
    assertEvaluates(
        "$cosh", Value(Decimal128("5.23598775598")), Value(Decimal128("93.9599750339")));
    assertEvaluates("$cosh", Value(Decimal128("5.49778714378")), Value(Decimal128("122.07757934")));
    assertEvaluates(
        "$cosh", Value(Decimal128("5.75958653158")), Value(Decimal128("158.610147472")));
    assertEvaluates(
        "$cosh", Value(Decimal128("6.28318530718")), Value(Decimal128("267.746761484")));
}

TEST(ExpressionHyperbolicCosineTest, NullArg) {
    assertEvaluates("$cosh", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionSine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of sin, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionSineTest, IntArg) {
    assertEvaluates("$sin", Value(0), Value(0.0));
    assertEvaluates("$sin", Value(1), Value(0.841470984808));
    assertEvaluates("$sin", Value(2), Value(0.909297426826));
    assertEvaluates("$sin", Value(3), Value(0.14112000806));
    assertEvaluates("$sin", Value(4), Value(-0.756802495308));
    assertEvaluates("$sin", Value(5), Value(-0.958924274663));
    assertEvaluates("$sin", Value(6), Value(-0.279415498199));
}

TEST(ExpressionSineTest, LongArg) {
    assertEvaluates("$sin", Value(0LL), Value(0.0));
    assertEvaluates("$sin", Value(1LL), Value(0.841470984808));
    assertEvaluates("$sin", Value(2LL), Value(0.909297426826));
    assertEvaluates("$sin", Value(3LL), Value(0.14112000806));
    assertEvaluates("$sin", Value(4LL), Value(-0.756802495308));
    assertEvaluates("$sin", Value(5LL), Value(-0.958924274663));
    assertEvaluates("$sin", Value(6LL), Value(-0.279415498199));
}

TEST(ExpressionSineTest, DoubleArg) {
    assertEvaluates("$sin", Value(0.0), Value(0.0));
    assertEvaluates("$sin", Value(0.523598775598), Value(0.5));
    assertEvaluates("$sin", Value(0.785398163397), Value(0.707106781187));
    assertEvaluates("$sin", Value(1.0471975512), Value(0.866025403784));
    assertEvaluates("$sin", Value(1.57079632679), Value(1.0));
    assertEvaluates("$sin", Value(2.09439510239), Value(0.866025403784));
    assertEvaluates("$sin", Value(2.35619449019), Value(0.707106781187));
    assertEvaluates("$sin", Value(2.61799387799), Value(0.5));
    assertEvaluates("$sin", Value(3.14159265359), Value(1.22464679915e-16));
    assertEvaluates("$sin", Value(3.66519142919), Value(-0.5));
    assertEvaluates("$sin", Value(3.92699081699), Value(-0.707106781187));
    assertEvaluates("$sin", Value(4.18879020479), Value(-0.866025403784));
    assertEvaluates("$sin", Value(4.71238898038), Value(-1.0));
    assertEvaluates("$sin", Value(5.23598775598), Value(-0.866025403784));
    assertEvaluates("$sin", Value(5.49778714378), Value(-0.707106781187));
    assertEvaluates("$sin", Value(5.75958653158), Value(-0.5));
    assertEvaluates("$sin", Value(6.28318530718), Value(-2.44929359829e-16));
}

TEST(ExpressionSineTest, DecimalArg) {
    assertEvaluates("$sin", Value(Decimal128("0.0")), Value(Decimal128("0.0")));
    assertEvaluates("$sin", Value(Decimal128("0.523598775598")), Value(Decimal128("0.5")));
    assertEvaluates(
        "$sin", Value(Decimal128("0.785398163397")), Value(Decimal128("0.707106781187")));
    assertEvaluates("$sin", Value(Decimal128("1.0471975512")), Value(Decimal128("0.866025403784")));
    assertEvaluates("$sin", Value(Decimal128("1.57079632679")), Value(Decimal128("1.0")));
    assertEvaluates(
        "$sin", Value(Decimal128("2.09439510239")), Value(Decimal128("0.866025403784")));
    assertEvaluates(
        "$sin", Value(Decimal128("2.35619449019")), Value(Decimal128("0.707106781187")));
    assertEvaluates("$sin", Value(Decimal128("2.61799387799")), Value(Decimal128("0.5")));
    assertEvaluates(
        "$sin", Value(Decimal128("3.14159265359")), Value(Decimal128("1.22464679915e-16")));
    assertEvaluates("$sin", Value(Decimal128("3.66519142919")), Value(Decimal128("-0.5")));
    assertEvaluates(
        "$sin", Value(Decimal128("3.92699081699")), Value(Decimal128("-0.707106781187")));
    assertEvaluates(
        "$sin", Value(Decimal128("4.18879020479")), Value(Decimal128("-0.866025403784")));
    assertEvaluates("$sin", Value(Decimal128("4.71238898038")), Value(Decimal128("-1.0")));
    assertEvaluates(
        "$sin", Value(Decimal128("5.23598775598")), Value(Decimal128("-0.866025403784")));
    assertEvaluates(
        "$sin", Value(Decimal128("5.49778714378")), Value(Decimal128("-0.707106781187")));
    assertEvaluates("$sin", Value(Decimal128("5.75958653158")), Value(Decimal128("-0.5")));
    assertEvaluates(
        "$sin", Value(Decimal128("6.28318530718")), Value(Decimal128("-2.44929359829e-16")));
}

TEST(ExpressionSineTest, NullArg) {
    assertEvaluates("$sin", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionHyperbolicSine -------------------------- */
/*
 * Test values were generated using the 64 bit std version of sinh, and help
 * ensure that we are calling the correct functions.
 */

TEST(ExpressionHyperbolicSineTest, IntArg) {
    assertEvaluates("$sinh", Value(0), Value(0.0));
    assertEvaluates("$sinh", Value(1), Value(1.17520119364));
    assertEvaluates("$sinh", Value(2), Value(3.62686040785));
    assertEvaluates("$sinh", Value(3), Value(10.0178749274));
    assertEvaluates("$sinh", Value(4), Value(27.2899171971));
    assertEvaluates("$sinh", Value(5), Value(74.2032105778));
    assertEvaluates("$sinh", Value(6), Value(201.71315737));
}

TEST(ExpressionHyperbolicSineTest, LongArg) {
    assertEvaluates("$sinh", Value(0LL), Value(0.0));
    assertEvaluates("$sinh", Value(1LL), Value(1.17520119364));
    assertEvaluates("$sinh", Value(2LL), Value(3.62686040785));
    assertEvaluates("$sinh", Value(3LL), Value(10.0178749274));
    assertEvaluates("$sinh", Value(4LL), Value(27.2899171971));
    assertEvaluates("$sinh", Value(5LL), Value(74.2032105778));
    assertEvaluates("$sinh", Value(6LL), Value(201.71315737));
}

TEST(ExpressionHyperbolicSineTest, DoubleArg) {
    assertEvaluates("$sinh", Value(0.0), Value(0.0));
    assertEvaluates("$sinh", Value(0.523598775598), Value(0.547853473888));
    assertEvaluates("$sinh", Value(0.785398163397), Value(0.868670961486));
    assertEvaluates("$sinh", Value(1.0471975512), Value(1.24936705052));
    assertEvaluates("$sinh", Value(1.57079632679), Value(2.30129890231));
    assertEvaluates("$sinh", Value(2.09439510239), Value(3.9986913428));
    assertEvaluates("$sinh", Value(2.35619449019), Value(5.22797192468));
    assertEvaluates("$sinh", Value(2.61799387799), Value(6.81762330413));
    assertEvaluates("$sinh", Value(3.14159265359), Value(11.5487393573));
    assertEvaluates("$sinh", Value(3.66519142919), Value(19.5190070464));
    assertEvaluates("$sinh", Value(3.92699081699), Value(25.3671583194));
    assertEvaluates("$sinh", Value(4.18879020479), Value(32.9639002901));
    assertEvaluates("$sinh", Value(4.71238898038), Value(55.6543975994));
    assertEvaluates("$sinh", Value(5.23598775598), Value(93.9546534685));
    assertEvaluates("$sinh", Value(5.49778714378), Value(122.073483515));
    assertEvaluates("$sinh", Value(5.75958653158), Value(158.606995057));
    assertEvaluates("$sinh", Value(6.28318530718), Value(267.744894041));
}

TEST(ExpressionHyperbolicSineTest, DecimalArg) {
    assertEvaluates("$sinh", Value(Decimal128("0.0")), Value(Decimal128("0.0")));
    assertEvaluates(
        "$sinh", Value(Decimal128("0.523598775598")), Value(Decimal128("0.547853473888")));
    assertEvaluates(
        "$sinh", Value(Decimal128("0.785398163397")), Value(Decimal128("0.868670961486")));
    assertEvaluates("$sinh", Value(Decimal128("1.0471975512")), Value(Decimal128("1.24936705052")));
    assertEvaluates(
        "$sinh", Value(Decimal128("1.57079632679")), Value(Decimal128("2.30129890231")));
    assertEvaluates("$sinh", Value(Decimal128("2.09439510239")), Value(Decimal128("3.9986913428")));
    assertEvaluates(
        "$sinh", Value(Decimal128("2.35619449019")), Value(Decimal128("5.22797192468")));
    assertEvaluates(
        "$sinh", Value(Decimal128("2.61799387799")), Value(Decimal128("6.81762330413")));
    assertEvaluates(
        "$sinh", Value(Decimal128("3.14159265359")), Value(Decimal128("11.5487393573")));
    assertEvaluates(
        "$sinh", Value(Decimal128("3.66519142919")), Value(Decimal128("19.5190070464")));
    assertEvaluates(
        "$sinh", Value(Decimal128("3.92699081699")), Value(Decimal128("25.3671583194")));
    assertEvaluates(
        "$sinh", Value(Decimal128("4.18879020479")), Value(Decimal128("32.9639002901")));
    assertEvaluates(
        "$sinh", Value(Decimal128("4.71238898038")), Value(Decimal128("55.6543975994")));
    assertEvaluates(
        "$sinh", Value(Decimal128("5.23598775598")), Value(Decimal128("93.9546534685")));
    assertEvaluates(
        "$sinh", Value(Decimal128("5.49778714378")), Value(Decimal128("122.073483515")));
    assertEvaluates(
        "$sinh", Value(Decimal128("5.75958653158")), Value(Decimal128("158.606995057")));
    assertEvaluates(
        "$sinh", Value(Decimal128("6.28318530718")), Value(Decimal128("267.744894041")));
}

TEST(ExpressionHyperbolicSineTest, NullArg) {
    assertEvaluates("$sinh", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionTangent -------------------------- */
/**
 * Test values were generated using the 64 bit std version of tan, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionTangentTest, IntArg) {
    assertEvaluates("$tan", Value(-1), Value(-1.55740772465));
    assertEvaluates("$tan", Value(0), Value(0.0));
    assertEvaluates("$tan", Value(1), Value(1.55740772465));
}

TEST(ExpressionTangentTest, LongArg) {
    assertEvaluates("$tan", Value(-1LL), Value(-1.55740772465));
    assertEvaluates("$tan", Value(0LL), Value(0.0));
    assertEvaluates("$tan", Value(1LL), Value(1.55740772465));
}

TEST(ExpressionTangentTest, DoubleArg) {
    assertEvaluates("$tan", Value(-1.5), Value(-14.1014199472));
    assertEvaluates("$tan", Value(-1.0471975512), Value(-1.73205080757));
    assertEvaluates("$tan", Value(-0.785398163397), Value(-1.0));
    assertEvaluates("$tan", Value(0), Value(0.0));
    assertEvaluates("$tan", Value(0.785398163397), Value(1.0));
    assertEvaluates("$tan", Value(1.0471975512), Value(1.73205080757));
    assertEvaluates("$tan", Value(1.5), Value(14.1014199472));
}

TEST(ExpressionTangentTest, DecimalArg) {
    assertEvaluates("$tan", Value(Decimal128("-1.5")), Value(Decimal128("-14.1014199472")));
    assertEvaluates(
        "$tan", Value(Decimal128("-1.0471975512")), Value(Decimal128("-1.73205080757")));
    assertEvaluates("$tan", Value(Decimal128("-0.785398163397")), Value(Decimal128("-1.0")));
    assertEvaluates("$tan", Value(Decimal128("0")), Value(Decimal128("0.0")));
    assertEvaluates("$tan", Value(Decimal128("0.785398163397")), Value(Decimal128("1.0")));
    assertEvaluates("$tan", Value(Decimal128("1.0471975512")), Value(Decimal128("1.73205080757")));
    assertEvaluates("$tan", Value(Decimal128("1.5")), Value(Decimal128("14.1014199472")));
}

TEST(ExpressionTangentTest, NullArg) {
    assertEvaluates("$tan", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionHyperbolicTangent -------------------------- */
/**
 * Test values were generated using the 64 bit std version of tanh, and help
 * ensure that we are calling the correct functions.
 *
 */
TEST(ExpressionHyperbolicTangentTest, IntArg) {
    assertEvaluates("$tanh", Value(0), Value(0.0));
    assertEvaluates("$tanh", Value(1), Value(0.761594155956));
    assertEvaluates("$tanh", Value(2), Value(0.964027580076));
    assertEvaluates("$tanh", Value(3), Value(0.995054753687));
    assertEvaluates("$tanh", Value(4), Value(0.999329299739));
    assertEvaluates("$tanh", Value(5), Value(0.999909204263));
    assertEvaluates("$tanh", Value(6), Value(0.999987711651));
}

TEST(ExpressionHyperbolicTangentTest, LongArg) {
    assertEvaluates("$tanh", Value(0LL), Value(0.0));
    assertEvaluates("$tanh", Value(1LL), Value(0.761594155956));
    assertEvaluates("$tanh", Value(2LL), Value(0.964027580076));
    assertEvaluates("$tanh", Value(3LL), Value(0.995054753687));
    assertEvaluates("$tanh", Value(4LL), Value(0.999329299739));
    assertEvaluates("$tanh", Value(5LL), Value(0.999909204263));
    assertEvaluates("$tanh", Value(6LL), Value(0.999987711651));
}

TEST(ExpressionHyperbolicTangentTest, DoubleArg) {
    assertEvaluates("$tanh", Value(0.0), Value(0.0));
    assertEvaluates("$tanh", Value(0.523598775598), Value(0.480472778156));
    assertEvaluates("$tanh", Value(0.785398163397), Value(0.655794202633));
    assertEvaluates("$tanh", Value(1.0471975512), Value(0.780714435359));
    assertEvaluates("$tanh", Value(1.57079632679), Value(0.917152335667));
    assertEvaluates("$tanh", Value(2.09439510239), Value(0.970123821166));
    assertEvaluates("$tanh", Value(2.35619449019), Value(0.982193380007));
    assertEvaluates("$tanh", Value(2.61799387799), Value(0.989413207353));
    assertEvaluates("$tanh", Value(3.14159265359), Value(0.996272076221));
    assertEvaluates("$tanh", Value(3.66519142919), Value(0.998690213046));
    assertEvaluates("$tanh", Value(3.92699081699), Value(0.999223894879));
    assertEvaluates("$tanh", Value(4.18879020479), Value(0.999540174353));
    assertEvaluates("$tanh", Value(4.71238898038), Value(0.999838613989));
    assertEvaluates("$tanh", Value(5.23598775598), Value(0.999943363486));
    assertEvaluates("$tanh", Value(5.49778714378), Value(0.999966449));
    assertEvaluates("$tanh", Value(5.75958653158), Value(0.99998012476));
    assertEvaluates("$tanh", Value(6.28318530718), Value(0.99999302534));
}

TEST(ExpressionHyperbolicTangentTest, DecimalArg) {
    assertEvaluates("$tanh", Value(Decimal128("0.0")), Value(Decimal128("0.0")));
    assertEvaluates(
        "$tanh", Value(Decimal128("0.523598775598")), Value(Decimal128("0.480472778156")));
    assertEvaluates(
        "$tanh", Value(Decimal128("0.785398163397")), Value(Decimal128("0.655794202633")));
    assertEvaluates(
        "$tanh", Value(Decimal128("1.0471975512")), Value(Decimal128("0.780714435359")));
    assertEvaluates(
        "$tanh", Value(Decimal128("1.57079632679")), Value(Decimal128("0.917152335667")));
    assertEvaluates(
        "$tanh", Value(Decimal128("2.09439510239")), Value(Decimal128("0.970123821166")));
    assertEvaluates(
        "$tanh", Value(Decimal128("2.35619449019")), Value(Decimal128("0.982193380007")));
    assertEvaluates(
        "$tanh", Value(Decimal128("2.61799387799")), Value(Decimal128("0.989413207353")));
    assertEvaluates(
        "$tanh", Value(Decimal128("3.14159265359")), Value(Decimal128("0.996272076221")));
    assertEvaluates(
        "$tanh", Value(Decimal128("3.66519142919")), Value(Decimal128("0.998690213046")));
    assertEvaluates(
        "$tanh", Value(Decimal128("3.92699081699")), Value(Decimal128("0.999223894879")));
    assertEvaluates(
        "$tanh", Value(Decimal128("4.18879020479")), Value(Decimal128("0.999540174353")));
    assertEvaluates(
        "$tanh", Value(Decimal128("4.71238898038")), Value(Decimal128("0.999838613989")));
    assertEvaluates(
        "$tanh", Value(Decimal128("5.23598775598")), Value(Decimal128("0.999943363486")));
    assertEvaluates("$tanh", Value(Decimal128("5.49778714378")), Value(Decimal128("0.999966449")));
    assertEvaluates(
        "$tanh", Value(Decimal128("5.75958653158")), Value(Decimal128("0.99998012476")));
    assertEvaluates(
        "$tanh", Value(Decimal128("6.28318530718")), Value(Decimal128("0.99999302534")));
}

TEST(ExpressionHyperbolicTangentTest, NullArg) {
    assertEvaluates("$tanh", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionHyperbolicArcCosine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of acosh, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionHyperbolicArcCosineTest, IntArg) {
    assertEvaluates("$acosh", Value(1), Value(0.000000));
    assertEvaluates("$acosh", Value(2), Value(1.316958));
    assertEvaluates("$acosh", Value(3), Value(1.762747));
    assertEvaluates("$acosh", Value(4), Value(2.063437));
    assertEvaluates("$acosh", Value(5), Value(2.292432));
    assertEvaluates("$acosh", Value(6), Value(2.477889));
    assertEvaluates("$acosh", Value(7), Value(2.633916));
    assertEvaluates("$acosh", Value(8), Value(2.768659));
    assertEvaluates("$acosh", Value(9), Value(2.887271));
}

TEST(ExpressionHyperbolicArcCosineTest, LongArg) {
    assertEvaluates("$acosh", Value(1LL), Value(0.000000));
    assertEvaluates("$acosh", Value(2LL), Value(1.316958));
    assertEvaluates("$acosh", Value(3LL), Value(1.762747));
    assertEvaluates("$acosh", Value(4LL), Value(2.063437));
    assertEvaluates("$acosh", Value(5LL), Value(2.292432));
    assertEvaluates("$acosh", Value(6LL), Value(2.477889));
    assertEvaluates("$acosh", Value(7LL), Value(2.633916));
    assertEvaluates("$acosh", Value(8LL), Value(2.768659));
    assertEvaluates("$acosh", Value(9LL), Value(2.887271));
}

TEST(ExpressionHyperbolicArcCosineTest, DoubleArg) {
    assertEvaluates("$acosh", Value(1.000000), Value(0.000000));
    assertEvaluates("$acosh", Value(1.200000), Value(0.622363));
    assertEvaluates("$acosh", Value(1.400000), Value(0.867015));
    assertEvaluates("$acosh", Value(1.600000), Value(1.046968));
    assertEvaluates("$acosh", Value(1.800000), Value(1.192911));
    assertEvaluates("$acosh", Value(2.000000), Value(1.316958));
    assertEvaluates("$acosh", Value(2.200000), Value(1.425417));
    assertEvaluates("$acosh", Value(2.400000), Value(1.522079));
    assertEvaluates("$acosh", Value(2.600000), Value(1.609438));
    assertEvaluates("$acosh", Value(2.800000), Value(1.689236));
    assertEvaluates("$acosh", Value(3.000000), Value(1.762747));
    assertEvaluates("$acosh", Value(3.200000), Value(1.830938));
    assertEvaluates("$acosh", Value(3.400000), Value(1.894559));
    assertEvaluates("$acosh", Value(3.600000), Value(1.954208));
    assertEvaluates("$acosh", Value(3.800000), Value(2.010367));
    assertEvaluates("$acosh", Value(4.000000), Value(2.063437));
    assertEvaluates("$acosh", Value(4.200000), Value(2.113748));
    assertEvaluates("$acosh", Value(4.400000), Value(2.161581));
    assertEvaluates("$acosh", Value(4.600000), Value(2.207174));
    assertEvaluates("$acosh", Value(4.800000), Value(2.250731));
    assertEvaluates("$acosh", Value(5.000000), Value(2.292432));
    assertEvaluates("$acosh", Value(5.200000), Value(2.332429));
    assertEvaluates("$acosh", Value(5.400000), Value(2.370860));
    assertEvaluates("$acosh", Value(5.600000), Value(2.407845));
    assertEvaluates("$acosh", Value(5.800000), Value(2.443489));
    assertEvaluates("$acosh", Value(6.000000), Value(2.477889));
    assertEvaluates("$acosh", Value(6.200000), Value(2.511128));
    assertEvaluates("$acosh", Value(6.400000), Value(2.543285));
    assertEvaluates("$acosh", Value(6.600000), Value(2.574428));
    assertEvaluates("$acosh", Value(6.800000), Value(2.604619));
    assertEvaluates("$acosh", Value(7.000000), Value(2.633916));
    assertEvaluates("$acosh", Value(7.200000), Value(2.662370));
    assertEvaluates("$acosh", Value(7.400000), Value(2.690030));
    assertEvaluates("$acosh", Value(7.600000), Value(2.716939));
    assertEvaluates("$acosh", Value(7.800000), Value(2.743136));
    assertEvaluates("$acosh", Value(8.000000), Value(2.768659));
    assertEvaluates("$acosh", Value(8.200000), Value(2.793542));
    assertEvaluates("$acosh", Value(8.400000), Value(2.817817));
    assertEvaluates("$acosh", Value(8.600000), Value(2.841512));
    assertEvaluates("$acosh", Value(8.800000), Value(2.864655));
    assertEvaluates("$acosh", Value(9.000000), Value(2.887271));
    assertEvaluates("$acosh", Value(9.200000), Value(2.909384));
    assertEvaluates("$acosh", Value(9.400000), Value(2.931015));
    assertEvaluates("$acosh", Value(9.600000), Value(2.952187));
    assertEvaluates("$acosh", Value(9.800000), Value(2.972916));
    assertEvaluates("$acosh", Value(10.000000), Value(2.993223));
}

TEST(ExpressionHyperbolicArcCosineTest, DecimalArg) {
    assertEvaluates("$acosh", Value(Decimal128(1.000000)), Value(Decimal128(0.000000)));
    assertEvaluates("$acosh", Value(Decimal128(1.200000)), Value(Decimal128(0.622363)));
    assertEvaluates("$acosh", Value(Decimal128(1.400000)), Value(Decimal128(0.867015)));
    assertEvaluates("$acosh", Value(Decimal128(1.600000)), Value(Decimal128(1.046968)));
    assertEvaluates("$acosh", Value(Decimal128(1.800000)), Value(Decimal128(1.192911)));
    assertEvaluates("$acosh", Value(Decimal128(2.000000)), Value(Decimal128(1.316958)));
    assertEvaluates("$acosh", Value(Decimal128(2.200000)), Value(Decimal128(1.425417)));
    assertEvaluates("$acosh", Value(Decimal128(2.400000)), Value(Decimal128(1.522079)));
    assertEvaluates("$acosh", Value(Decimal128(2.600000)), Value(Decimal128(1.609438)));
    assertEvaluates("$acosh", Value(Decimal128(2.800000)), Value(Decimal128(1.689236)));
    assertEvaluates("$acosh", Value(Decimal128(3.000000)), Value(Decimal128(1.762747)));
    assertEvaluates("$acosh", Value(Decimal128(3.200000)), Value(Decimal128(1.830938)));
    assertEvaluates("$acosh", Value(Decimal128(3.400000)), Value(Decimal128(1.894559)));
    assertEvaluates("$acosh", Value(Decimal128(3.600000)), Value(Decimal128(1.954208)));
    assertEvaluates("$acosh", Value(Decimal128(3.800000)), Value(Decimal128(2.010367)));
    assertEvaluates("$acosh", Value(Decimal128(4.000000)), Value(Decimal128(2.063437)));
    assertEvaluates("$acosh", Value(Decimal128(4.200000)), Value(Decimal128(2.113748)));
    assertEvaluates("$acosh", Value(Decimal128(4.400000)), Value(Decimal128(2.161581)));
    assertEvaluates("$acosh", Value(Decimal128(4.600000)), Value(Decimal128(2.207174)));
    assertEvaluates("$acosh", Value(Decimal128(4.800000)), Value(Decimal128(2.250731)));
    assertEvaluates("$acosh", Value(Decimal128(5.000000)), Value(Decimal128(2.292432)));
    assertEvaluates("$acosh", Value(Decimal128(5.200000)), Value(Decimal128(2.332429)));
    assertEvaluates("$acosh", Value(Decimal128(5.400000)), Value(Decimal128(2.370860)));
    assertEvaluates("$acosh", Value(Decimal128(5.600000)), Value(Decimal128(2.407845)));
    assertEvaluates("$acosh", Value(Decimal128(5.800000)), Value(Decimal128(2.443489)));
    assertEvaluates("$acosh", Value(Decimal128(6.000000)), Value(Decimal128(2.477889)));
    assertEvaluates("$acosh", Value(Decimal128(6.200000)), Value(Decimal128(2.511128)));
    assertEvaluates("$acosh", Value(Decimal128(6.400000)), Value(Decimal128(2.543285)));
    assertEvaluates("$acosh", Value(Decimal128(6.600000)), Value(Decimal128(2.574428)));
    assertEvaluates("$acosh", Value(Decimal128(6.800000)), Value(Decimal128(2.604619)));
    assertEvaluates("$acosh", Value(Decimal128(7.000000)), Value(Decimal128(2.633916)));
    assertEvaluates("$acosh", Value(Decimal128(7.200000)), Value(Decimal128(2.662370)));
    assertEvaluates("$acosh", Value(Decimal128(7.400000)), Value(Decimal128(2.690030)));
    assertEvaluates("$acosh", Value(Decimal128(7.600000)), Value(Decimal128(2.716939)));
    assertEvaluates("$acosh", Value(Decimal128(7.800000)), Value(Decimal128(2.743136)));
    assertEvaluates("$acosh", Value(Decimal128(8.000000)), Value(Decimal128(2.768659)));
    assertEvaluates("$acosh", Value(Decimal128(8.200000)), Value(Decimal128(2.793542)));
    assertEvaluates("$acosh", Value(Decimal128(8.400000)), Value(Decimal128(2.817817)));
    assertEvaluates("$acosh", Value(Decimal128(8.600000)), Value(Decimal128(2.841512)));
    assertEvaluates("$acosh", Value(Decimal128(8.800000)), Value(Decimal128(2.864655)));
    assertEvaluates("$acosh", Value(Decimal128(9.000000)), Value(Decimal128(2.887271)));
    assertEvaluates("$acosh", Value(Decimal128(9.200000)), Value(Decimal128(2.909384)));
    assertEvaluates("$acosh", Value(Decimal128(9.400000)), Value(Decimal128(2.931015)));
    assertEvaluates("$acosh", Value(Decimal128(9.600000)), Value(Decimal128(2.952187)));
    assertEvaluates("$acosh", Value(Decimal128(9.800000)), Value(Decimal128(2.972916)));
    assertEvaluates("$acosh", Value(Decimal128(10.000000)), Value(Decimal128(2.993223)));
}

TEST(ExpressionHyperbolicArcCosineTest, NullArg) {
    assertEvaluates("$acosh", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionHyperbolicArcSine -------------------------- */
/**
 * Test values were generated using the 64 bit std version of asinh, and help
 * ensure that we are calling the correct functions.
 *
 */

TEST(ExpressionHyperbolicArcSineTest, IntArg) {
    assertEvaluates("$asinh", Value(1), Value(0.881374));
    assertEvaluates("$asinh", Value(2), Value(1.443635));
    assertEvaluates("$asinh", Value(3), Value(1.818446));
    assertEvaluates("$asinh", Value(4), Value(2.094713));
    assertEvaluates("$asinh", Value(5), Value(2.312438));
    assertEvaluates("$asinh", Value(6), Value(2.491780));
    assertEvaluates("$asinh", Value(7), Value(2.644121));
    assertEvaluates("$asinh", Value(8), Value(2.776472));
    assertEvaluates("$asinh", Value(9), Value(2.893444));
}

TEST(ExpressionHyperbolicArcSineTest, LongArg) {
    assertEvaluates("$asinh", Value(1LL), Value(0.881374));
    assertEvaluates("$asinh", Value(2LL), Value(1.443635));
    assertEvaluates("$asinh", Value(3LL), Value(1.818446));
    assertEvaluates("$asinh", Value(4LL), Value(2.094713));
    assertEvaluates("$asinh", Value(5LL), Value(2.312438));
    assertEvaluates("$asinh", Value(6LL), Value(2.491780));
    assertEvaluates("$asinh", Value(7LL), Value(2.644121));
    assertEvaluates("$asinh", Value(8LL), Value(2.776472));
    assertEvaluates("$asinh", Value(9LL), Value(2.893444));
}

TEST(ExpressionHyperbolicArcSineTest, DoubleArg) {
    assertEvaluates("$asinh", Value(1.000000), Value(0.881374));
    assertEvaluates("$asinh", Value(1.200000), Value(1.015973));
    assertEvaluates("$asinh", Value(1.400000), Value(1.137982));
    assertEvaluates("$asinh", Value(1.600000), Value(1.248983));
    assertEvaluates("$asinh", Value(1.800000), Value(1.350441));
    assertEvaluates("$asinh", Value(2.000000), Value(1.443635));
    assertEvaluates("$asinh", Value(2.200000), Value(1.529660));
    assertEvaluates("$asinh", Value(2.400000), Value(1.609438));
    assertEvaluates("$asinh", Value(2.600000), Value(1.683743));
    assertEvaluates("$asinh", Value(2.800000), Value(1.753229));
    assertEvaluates("$asinh", Value(3.000000), Value(1.818446));
    assertEvaluates("$asinh", Value(3.200000), Value(1.879864));
    assertEvaluates("$asinh", Value(3.400000), Value(1.937879));
    assertEvaluates("$asinh", Value(3.600000), Value(1.992836));
    assertEvaluates("$asinh", Value(3.800000), Value(2.045028));
    assertEvaluates("$asinh", Value(4.000000), Value(2.094713));
    assertEvaluates("$asinh", Value(4.200000), Value(2.142112));
    assertEvaluates("$asinh", Value(4.400000), Value(2.187422));
    assertEvaluates("$asinh", Value(4.600000), Value(2.230814));
    assertEvaluates("$asinh", Value(4.800000), Value(2.272441));
    assertEvaluates("$asinh", Value(5.000000), Value(2.312438));
    assertEvaluates("$asinh", Value(5.200000), Value(2.350926));
    assertEvaluates("$asinh", Value(5.400000), Value(2.388011));
    assertEvaluates("$asinh", Value(5.600000), Value(2.423792));
    assertEvaluates("$asinh", Value(5.800000), Value(2.458355));
    assertEvaluates("$asinh", Value(6.000000), Value(2.491780));
    assertEvaluates("$asinh", Value(6.200000), Value(2.524138));
    assertEvaluates("$asinh", Value(6.400000), Value(2.555494));
    assertEvaluates("$asinh", Value(6.600000), Value(2.585907));
    assertEvaluates("$asinh", Value(6.800000), Value(2.615433));
    assertEvaluates("$asinh", Value(7.000000), Value(2.644121));
    assertEvaluates("$asinh", Value(7.200000), Value(2.672016));
    assertEvaluates("$asinh", Value(7.400000), Value(2.699162));
    assertEvaluates("$asinh", Value(7.600000), Value(2.725596));
    assertEvaluates("$asinh", Value(7.800000), Value(2.751355));
    assertEvaluates("$asinh", Value(8.000000), Value(2.776472));
    assertEvaluates("$asinh", Value(8.200000), Value(2.800979));
    assertEvaluates("$asinh", Value(8.400000), Value(2.824903));
    assertEvaluates("$asinh", Value(8.600000), Value(2.848273));
    assertEvaluates("$asinh", Value(8.800000), Value(2.871112));
    assertEvaluates("$asinh", Value(9.000000), Value(2.893444));
    assertEvaluates("$asinh", Value(9.200000), Value(2.915291));
    assertEvaluates("$asinh", Value(9.400000), Value(2.936674));
    assertEvaluates("$asinh", Value(9.600000), Value(2.957612));
    assertEvaluates("$asinh", Value(9.800000), Value(2.978123));
    assertEvaluates("$asinh", Value(10.000000), Value(2.998223));
}

TEST(ExpressionHyperbolicArcSineTest, DecimalArg) {
    assertEvaluates("$asinh", Value(Decimal128(1.000000)), Value(Decimal128(0.881374)));
    assertEvaluates("$asinh", Value(Decimal128(1.200000)), Value(Decimal128(1.015973)));
    assertEvaluates("$asinh", Value(Decimal128(1.400000)), Value(Decimal128(1.137982)));
    assertEvaluates("$asinh", Value(Decimal128(1.600000)), Value(Decimal128(1.248983)));
    assertEvaluates("$asinh", Value(Decimal128(1.800000)), Value(Decimal128(1.350441)));
    assertEvaluates("$asinh", Value(Decimal128(2.000000)), Value(Decimal128(1.443635)));
    assertEvaluates("$asinh", Value(Decimal128(2.200000)), Value(Decimal128(1.529660)));
    assertEvaluates("$asinh", Value(Decimal128(2.400000)), Value(Decimal128(1.609438)));
    assertEvaluates("$asinh", Value(Decimal128(2.600000)), Value(Decimal128(1.683743)));
    assertEvaluates("$asinh", Value(Decimal128(2.800000)), Value(Decimal128(1.753229)));
    assertEvaluates("$asinh", Value(Decimal128(3.000000)), Value(Decimal128(1.818446)));
    assertEvaluates("$asinh", Value(Decimal128(3.200000)), Value(Decimal128(1.879864)));
    assertEvaluates("$asinh", Value(Decimal128(3.400000)), Value(Decimal128(1.937879)));
    assertEvaluates("$asinh", Value(Decimal128(3.600000)), Value(Decimal128(1.992836)));
    assertEvaluates("$asinh", Value(Decimal128(3.800000)), Value(Decimal128(2.045028)));
    assertEvaluates("$asinh", Value(Decimal128(4.000000)), Value(Decimal128(2.094713)));
    assertEvaluates("$asinh", Value(Decimal128(4.200000)), Value(Decimal128(2.142112)));
    assertEvaluates("$asinh", Value(Decimal128(4.400000)), Value(Decimal128(2.187422)));
    assertEvaluates("$asinh", Value(Decimal128(4.600000)), Value(Decimal128(2.230814)));
    assertEvaluates("$asinh", Value(Decimal128(4.800000)), Value(Decimal128(2.272441)));
    assertEvaluates("$asinh", Value(Decimal128(5.000000)), Value(Decimal128(2.312438)));
    assertEvaluates("$asinh", Value(Decimal128(5.200000)), Value(Decimal128(2.350926)));
    assertEvaluates("$asinh", Value(Decimal128(5.400000)), Value(Decimal128(2.388011)));
    assertEvaluates("$asinh", Value(Decimal128(5.600000)), Value(Decimal128(2.423792)));
    assertEvaluates("$asinh", Value(Decimal128(5.800000)), Value(Decimal128(2.458355)));
    assertEvaluates("$asinh", Value(Decimal128(6.000000)), Value(Decimal128(2.491780)));
    assertEvaluates("$asinh", Value(Decimal128(6.200000)), Value(Decimal128(2.524138)));
    assertEvaluates("$asinh", Value(Decimal128(6.400000)), Value(Decimal128(2.555494)));
    assertEvaluates("$asinh", Value(Decimal128(6.600000)), Value(Decimal128(2.585907)));
    assertEvaluates("$asinh", Value(Decimal128(6.800000)), Value(Decimal128(2.615433)));
    assertEvaluates("$asinh", Value(Decimal128(7.000000)), Value(Decimal128(2.644121)));
    assertEvaluates("$asinh", Value(Decimal128(7.200000)), Value(Decimal128(2.672016)));
    assertEvaluates("$asinh", Value(Decimal128(7.400000)), Value(Decimal128(2.699162)));
    assertEvaluates("$asinh", Value(Decimal128(7.600000)), Value(Decimal128(2.725596)));
    assertEvaluates("$asinh", Value(Decimal128(7.800000)), Value(Decimal128(2.751355)));
    assertEvaluates("$asinh", Value(Decimal128(8.000000)), Value(Decimal128(2.776472)));
    assertEvaluates("$asinh", Value(Decimal128(8.200000)), Value(Decimal128(2.800979)));
    assertEvaluates("$asinh", Value(Decimal128(8.400000)), Value(Decimal128(2.824903)));
    assertEvaluates("$asinh", Value(Decimal128(8.600000)), Value(Decimal128(2.848273)));
    assertEvaluates("$asinh", Value(Decimal128(8.800000)), Value(Decimal128(2.871112)));
    assertEvaluates("$asinh", Value(Decimal128(9.000000)), Value(Decimal128(2.893444)));
    assertEvaluates("$asinh", Value(Decimal128(9.200000)), Value(Decimal128(2.915291)));
    assertEvaluates("$asinh", Value(Decimal128(9.400000)), Value(Decimal128(2.936674)));
    assertEvaluates("$asinh", Value(Decimal128(9.600000)), Value(Decimal128(2.957612)));
    assertEvaluates("$asinh", Value(Decimal128(9.800000)), Value(Decimal128(2.978123)));
    assertEvaluates("$asinh", Value(Decimal128(10.000000)), Value(Decimal128(2.998223)));
}

TEST(ExpressionHyperbolicArcSineTest, NullArg) {
    assertEvaluates("$asinh", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionHyperbolicArcTangent -------------------------- */
/**
 * Test values were generated using the 64 bit std version of tanh, and help
 * ensure that we are calling the correct functions.
 *
 */
TEST(ExpressionHyperbolicArcTangentTest, IntArg) {
    assertEvaluates("$atanh", Value(0), Value(0.000000));
}

TEST(ExpressionHyperbolicArcTangentTest, LongArg) {
    assertEvaluates("$atanh", Value(0LL), Value(0.000000));
}

TEST(ExpressionHyperbolicArcTangentTest, DoubleArg) {
    assertEvaluates("$atanh", Value(-0.990000), Value(-2.646652));
    assertEvaluates("$atanh", Value(-0.790000), Value(-1.071432));
    assertEvaluates("$atanh", Value(-0.590000), Value(-0.677666));
    assertEvaluates("$atanh", Value(-0.390000), Value(-0.411800));
    assertEvaluates("$atanh", Value(-0.190000), Value(-0.192337));
    assertEvaluates("$atanh", Value(0.010000), Value(0.010000));
    assertEvaluates("$atanh", Value(0.210000), Value(0.213171));
    assertEvaluates("$atanh", Value(0.410000), Value(0.435611));
    assertEvaluates("$atanh", Value(0.610000), Value(0.708921));
    assertEvaluates("$atanh", Value(0.810000), Value(1.127029));
}

TEST(ExpressionHyperbolicArcTangentTest, DecimalArg) {
    assertEvaluates("$atanh", Value(Decimal128(-0.990000)), Value(Decimal128(-2.646652)));
    assertEvaluates("$atanh", Value(Decimal128(-0.790000)), Value(Decimal128(-1.071432)));
    assertEvaluates("$atanh", Value(Decimal128(-0.590000)), Value(Decimal128(-0.677666)));
    assertEvaluates("$atanh", Value(Decimal128(-0.390000)), Value(Decimal128(-0.411800)));
    assertEvaluates("$atanh", Value(Decimal128(-0.190000)), Value(Decimal128(-0.192337)));
    assertEvaluates("$atanh", Value(Decimal128(0.010000)), Value(Decimal128(0.010000)));
    assertEvaluates("$atanh", Value(Decimal128(0.210000)), Value(Decimal128(0.213171)));
    assertEvaluates("$atanh", Value(Decimal128(0.410000)), Value(Decimal128(0.435611)));
    assertEvaluates("$atanh", Value(Decimal128(0.610000)), Value(Decimal128(0.708921)));
    assertEvaluates("$atanh", Value(Decimal128(0.810000)), Value(Decimal128(1.127029)));
}

TEST(ExpressionHyperbolicArcTangentTest, NullArg) {
    assertEvaluates("$atanh", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionRadiansToDegrees -------------------------- */
TEST(ExpressionRadiansToDegreesTest, IntArg) {
    assertEvaluates("$radiansToDegrees", Value(0), Value(0.0));
    assertEvaluates("$radiansToDegrees", Value(1), Value(57.2957795131));
    assertEvaluates("$radiansToDegrees", Value(2), Value(114.591559026));
    assertEvaluates("$radiansToDegrees", Value(3), Value(171.887338539));
    assertEvaluates("$radiansToDegrees", Value(4), Value(229.183118052));
    assertEvaluates("$radiansToDegrees", Value(5), Value(286.478897565));
    assertEvaluates("$radiansToDegrees", Value(6), Value(343.774677078));
}

TEST(ExpressionRadiansToDegreesTest, LongArg) {
    assertEvaluates("$radiansToDegrees", Value(0LL), Value(0.0));
    assertEvaluates("$radiansToDegrees", Value(1LL), Value(57.2957795131));
    assertEvaluates("$radiansToDegrees", Value(2LL), Value(114.591559026));
    assertEvaluates("$radiansToDegrees", Value(3LL), Value(171.887338539));
    assertEvaluates("$radiansToDegrees", Value(4LL), Value(229.183118052));
    assertEvaluates("$radiansToDegrees", Value(5LL), Value(286.478897565));
    assertEvaluates("$radiansToDegrees", Value(6LL), Value(343.774677078));
}

TEST(ExpressionRadiansToDegreesTest, DoubleArg) {
    assertEvaluates("$radiansToDegrees", Value(0.0), Value(0.0));
    assertEvaluates("$radiansToDegrees", Value(0.523598775598), Value(30.0));
    assertEvaluates("$radiansToDegrees", Value(0.785398163397), Value(45.0));
    assertEvaluates("$radiansToDegrees", Value(1.0471975512), Value(60.0));
    assertEvaluates("$radiansToDegrees", Value(1.57079632679), Value(90.0));
    assertEvaluates("$radiansToDegrees", Value(2.09439510239), Value(120.0));
    assertEvaluates("$radiansToDegrees", Value(2.35619449019), Value(135.0));
    assertEvaluates("$radiansToDegrees", Value(2.61799387799), Value(150.0));
    assertEvaluates("$radiansToDegrees", Value(3.14159265359), Value(180.0));
    assertEvaluates("$radiansToDegrees", Value(3.66519142919), Value(210.0));
    assertEvaluates("$radiansToDegrees", Value(3.92699081699), Value(225.0));
    assertEvaluates("$radiansToDegrees", Value(4.18879020479), Value(240.0));
    assertEvaluates("$radiansToDegrees", Value(4.71238898038), Value(270.0));
    assertEvaluates("$radiansToDegrees", Value(5.23598775598), Value(300.0));
    assertEvaluates("$radiansToDegrees", Value(5.49778714378), Value(315.0));
    assertEvaluates("$radiansToDegrees", Value(5.75958653158), Value(330.0));
    assertEvaluates("$radiansToDegrees", Value(6.28318530718), Value(360.0));
}

TEST(ExpressionRadiansToDegreesTest, DecimalArg) {
    assertEvaluates("$radiansToDegrees", Value(Decimal128("0.0")), Value(Decimal128("0.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("0.523598775598")), Value(Decimal128("30.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("0.785398163397")), Value(Decimal128("45.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("1.0471975512")), Value(Decimal128("60.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("1.57079632679")), Value(Decimal128("90.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("2.09439510239")), Value(Decimal128("120.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("2.35619449019")), Value(Decimal128("135.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("2.61799387799")), Value(Decimal128("150.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("3.14159265359")), Value(Decimal128("180.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("3.66519142919")), Value(Decimal128("210.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("3.92699081699")), Value(Decimal128("225.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("4.18879020479")), Value(Decimal128("240.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("4.71238898038")), Value(Decimal128("270.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("5.23598775598")), Value(Decimal128("300.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("5.49778714378")), Value(Decimal128("315.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("5.75958653158")), Value(Decimal128("330.0")));
    assertEvaluates(
        "$radiansToDegrees", Value(Decimal128("6.28318530718")), Value(Decimal128("360.0")));
}

TEST(ExpressionRadiansToDegreesTest, NullArg) {
    assertEvaluates("$radiansToDegrees", Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionDegreesToRadians -------------------------- */
TEST(ExpressionDegreesToRadiansTest, IntArg) {
    assertEvaluates("$degreesToRadians", Value(0), Value(0.0));
    assertEvaluates("$degreesToRadians", Value(45), Value(0.785398163397));
    assertEvaluates("$degreesToRadians", Value(90), Value(1.57079632679));
    assertEvaluates("$degreesToRadians", Value(135), Value(2.35619449019));
    assertEvaluates("$degreesToRadians", Value(180), Value(3.14159265359));
    assertEvaluates("$degreesToRadians", Value(225), Value(3.92699081699));
    assertEvaluates("$degreesToRadians", Value(270), Value(4.71238898038));
    assertEvaluates("$degreesToRadians", Value(315), Value(5.49778714378));
    assertEvaluates("$degreesToRadians", Value(360), Value(6.28318530718));
}

TEST(ExpressionDegreesToRadiansTest, LongArg) {
    assertEvaluates("$degreesToRadians", Value(0LL), Value(0.0));
    assertEvaluates("$degreesToRadians", Value(45LL), Value(0.785398163397));
    assertEvaluates("$degreesToRadians", Value(90LL), Value(1.57079632679));
    assertEvaluates("$degreesToRadians", Value(135LL), Value(2.35619449019));
    assertEvaluates("$degreesToRadians", Value(180LL), Value(3.14159265359));
    assertEvaluates("$degreesToRadians", Value(225LL), Value(3.92699081699));
    assertEvaluates("$degreesToRadians", Value(270LL), Value(4.71238898038));
    assertEvaluates("$degreesToRadians", Value(315LL), Value(5.49778714378));
    assertEvaluates("$degreesToRadians", Value(360LL), Value(6.28318530718));
}

TEST(ExpressionDegreesToRadiansTest, DoubleArg) {
    assertEvaluates("$degreesToRadians", Value(0), Value(0.0));
    assertEvaluates("$degreesToRadians", Value(45), Value(0.785398163397));
    assertEvaluates("$degreesToRadians", Value(90), Value(1.57079632679));
    assertEvaluates("$degreesToRadians", Value(135), Value(2.35619449019));
    assertEvaluates("$degreesToRadians", Value(180), Value(3.14159265359));
    assertEvaluates("$degreesToRadians", Value(225), Value(3.92699081699));
    assertEvaluates("$degreesToRadians", Value(270), Value(4.71238898038));
    assertEvaluates("$degreesToRadians", Value(315), Value(5.49778714378));
    assertEvaluates("$degreesToRadians", Value(360), Value(6.28318530718));
}

TEST(ExpressionDegreesToRadiansTest, DecimalArg) {
    assertEvaluates("$degreesToRadians", Value(Decimal128("0")), Value(Decimal128("0.0")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("45")), Value(Decimal128("0.785398163397")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("90")), Value(Decimal128("1.57079632679")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("135")), Value(Decimal128("2.35619449019")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("180")), Value(Decimal128("3.14159265359")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("225")), Value(Decimal128("3.92699081699")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("270")), Value(Decimal128("4.71238898038")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("315")), Value(Decimal128("5.49778714378")));
    assertEvaluates(
        "$degreesToRadians", Value(Decimal128("360")), Value(Decimal128("6.28318530718")));
}

TEST(ExpressionDegreesToRadiansTest, NullArg) {
    assertEvaluates("$degreesToRadians", Value(BSONNULL), Value(BSONNULL));
}
}  // namespace expression_tests
