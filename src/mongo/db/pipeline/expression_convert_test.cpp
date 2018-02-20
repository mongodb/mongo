/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace ExpressionConvertTest {

using ExpressionConvertTest = AggregationContextFixture;

TEST_F(ExpressionConvertTest, ParseAndSerializeWithoutOptionalArguments) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}}}")),
                    convertExp->serialize(false));

    ASSERT_VALUE_EQ(Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}}}")),
                    convertExp->serialize(true));
}

TEST_F(ExpressionConvertTest, ParseAndSerializeWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"
                                        << "onError"
                                        << 0));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onError: {$const: 0}}}")),
        convertExp->serialize(false));

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onError: {$const: 0}}}")),
        convertExp->serialize(true));
}

TEST_F(ExpressionConvertTest, ParseAndSerializeWithOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"
                                        << "onNull"
                                        << 0));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onNull: {$const: 0}}}")),
        convertExp->serialize(false));

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onNull: {$const: 0}}}")),
        convertExp->serialize(true));
}

TEST_F(ExpressionConvertTest, ConvertWithoutInputFailsToParse) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("to"
                                        << "int"
                                        << "onError"
                                        << 0));
    ASSERT_THROWS_WITH_CHECK(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Missing 'input' parameter to $convert");
                             });
}

TEST_F(ExpressionConvertTest, ConvertWithoutToFailsToParse) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "onError"
                                        << 0));
    ASSERT_THROWS_WITH_CHECK(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Missing 'to' parameter to $convert");
                             });
}

TEST_F(ExpressionConvertTest, InvalidTypeNameFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "dinosaur"
                                        << "onError"
                                        << 0));

    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document()),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::BadValue);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Unknown type name");
                             });
}

TEST_F(ExpressionConvertTest, NonIntegralTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << 3.6
                                        << "onError"
                                        << 0));

    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document()),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "In $convert, numeric 'to' argument is not an integer");
                             });
}

TEST_F(ExpressionConvertTest, NonStringNonNumericalTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << BSON("dinosaur"
                                                << "Tyrannosaurus rex")
                                        << "onError"
                                        << 0));

    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document()),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "$convert's 'to' argument must be a string or number");
                             });
}

TEST_F(ExpressionConvertTest, IllegalTargetTypeFails) {
    auto expCtx = getExpCtx();

    std::vector<std::string> illegalTargetTypes{"minKey",
                                                "object",
                                                "array",
                                                "binData",
                                                "undefined",
                                                "null",
                                                "regex",
                                                "dbPointer",
                                                "javascript",
                                                "symbol",
                                                "javascriptWithScope",
                                                "timestamp",
                                                "maxKey"};

    // Attempt a conversion with each illegal type.
    for (auto&& typeName : illegalTargetTypes) {
        auto spec = BSON("$convert" << BSON("input"
                                            << "$path1"
                                            << "to"
                                            << Value(typeName)
                                            << "onError"
                                            << 0));

        auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

        ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document()),
                                 AssertionException,
                                 [](const AssertionException& exception) {
                                     ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                     ASSERT_STRING_CONTAINS(exception.reason(),
                                                            "$convert with unsupported 'to' type");
                                 });
    }
}

TEST_F(ExpressionConvertTest, InvalidNumericTargetTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << 100
                                        << "onError"
                                        << 0));

    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(Document()),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "In $convert, numeric value for 'to' does not correspond to a BSON type");
        });
}

TEST_F(ExpressionConvertTest, NegativeNumericTargetTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << -2
                                        << "onError"
                                        << 0));

    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(Document()),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "In $convert, numeric value for 'to' does not correspond to a BSON type");
        });
}

TEST_F(ExpressionConvertTest, UnsupportedConversionFails) {
    auto expCtx = getExpCtx();

    std::vector<std::pair<Value, std::string>> unsupportedConversions{
        {Value(OID()), "double"},
        {Value(OID()), "int"},
        {Value(OID()), "long"},
        {Value(OID()), "decimal"},
        {Value(Date_t::fromMillisSinceEpoch(0)), "objectId"},
        {Value(0.0), "date"},
        {Value(int{1}), "date"},
        {Value(true), "date"},
        {Value(0LL), "date"},
        {Value(Decimal128("0")), "date"},
    };

    // Attempt every possible unsupported conversion.
    for (auto conversion : unsupportedConversions) {
        auto inputValue = conversion.first;
        auto targetTypeName = conversion.second;

        auto spec = BSON("$convert" << BSON("input"
                                            << "$path1"
                                            << "to"
                                            << Value(targetTypeName)));

        Document intInput{{"path1", inputValue}};

        auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

        ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(intInput),
                                 AssertionException,
                                 [](const AssertionException& exception) {
                                     ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                     ASSERT_STRING_CONTAINS(exception.reason(),
                                                            "Unsupported conversion");
                                 });
    }
}

TEST_F(ExpressionConvertTest, ConvertNullishInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document nullInput{{"path1", Value(BSONNULL)}};
    Document undefinedInput{{"path1", Value(BSONUndefined)}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput), Value(BSONNULL));
}

TEST_F(ExpressionConvertTest, ConvertNullishInputWithOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"
                                        << "onNull"
                                        << "B)"
                                        << "onError"
                                        << "Should not be used here"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document nullInput{{"path1", Value(BSONNULL)}};
    Document undefinedInput{{"path1", Value(BSONUndefined)}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput), Value("B)"_sd));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput), Value("B)"_sd));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput), Value("B)"_sd));
}

TEST_F(ExpressionConvertTest, NullishToReturnsNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "inputString"
                                        << "to"
                                        << "$path1"
                                        << "onNull"
                                        << "Should not be used here"
                                        << "onError"
                                        << "Also should not be used"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document nullInput{{"path1", Value(BSONNULL)}};
    Document undefinedInput{{"path1", Value(BSONUndefined)}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput), Value(BSONNULL));
}

#define ASSERT_VALUE_CONTENTS_AND_TYPE(v, contents, type)  \
    do {                                                   \
        Value evaluatedResult = v;                         \
        ASSERT_VALUE_EQ(evaluatedResult, Value(contents)); \
        ASSERT_EQ(evaluatedResult.getType(), type);        \
    } while (false);

TEST_F(ExpressionConvertTest, NullInputOverridesNullTo) {
    auto expCtx = getExpCtx();

    auto spec =
        BSON("$convert" << BSON("input" << Value(BSONNULL) << "to" << Value(BSONNULL) << "onNull"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(Document{}), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertOptimizesToExpressionConstant) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << 0 << "to"
                                                << "double"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    convertExp = convertExp->optimize();

    auto constResult = dynamic_cast<ExpressionConstant*>(convertExp.get());
    ASSERT(constResult);
    ASSERT_VALUE_CONTENTS_AND_TYPE(constResult->getValue(), 0.0, BSONType::NumberDouble);
}

TEST_F(ExpressionConvertTest, ConvertWithOnErrorOptimizesToExpressionConstant) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << 0 << "to"
                                                << "objectId"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    convertExp = convertExp->optimize();

    auto constResult = dynamic_cast<ExpressionConstant*>(convertExp.get());
    ASSERT(constResult);
    ASSERT_VALUE_CONTENTS_AND_TYPE(constResult->getValue(), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, DoubleIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "double"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document doubleInput{{"path1", Value(2.4)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInput), 2.4, BSONType::NumberDouble);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    auto result = convertExp->evaluate(doubleNaN);
    ASSERT(std::isnan(result.getDouble()));

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    result = convertExp->evaluate(doubleInfinity);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_GT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    result = convertExp->evaluate(doubleNegativeInfinity);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_LT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));
}

TEST_F(ExpressionConvertTest, BoolIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueBoolInput{{"path1", Value(true)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueBoolInput), true, BSONType::Bool);

    Document falseBoolInput{{"path1", Value(false)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseBoolInput), false, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, IntIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document intInput{{"path1", Value(int{123})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(intInput), int{123}, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, LongIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document longInput{{"path1", Value(123LL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(longInput), 123LL, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, DecimalIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document decimalInput{{"path1", Value(Decimal128("2.4"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalInput), Decimal128("2.4"), BSONType::NumberDecimal);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNaN), Decimal128::kPositiveNaN, BSONType::NumberDecimal);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInfinity),
                                   Decimal128::kPositiveInfinity,
                                   BSONType::NumberDecimal);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalNegativeInfinity),
                                   Decimal128::kNegativeInfinity,
                                   BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertIntToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueIntInput{{"path1", Value(int{1})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueIntInput), true, BSONType::Bool);

    Document falseIntInput{{"path1", Value(int{0})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseIntInput), false, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertLongToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueLongInput{{"path1", Value(-1ll)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueLongInput), true, BSONType::Bool);

    Document falseLongInput{{"path1", Value(0ll)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseLongInput), false, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertDoubleToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueDoubleInput{{"path1", Value(2.4)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueDoubleInput), true, BSONType::Bool);

    Document falseDoubleInput{{"path1", Value(-0.0)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseDoubleInput), false, BSONType::Bool);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleNaN), true, BSONType::Bool);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInfinity), true, BSONType::Bool);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertDecimalToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueDecimalInput{{"path1", Value(Decimal128(5))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueDecimalInput), true, BSONType::Bool);

    Document falseDecimalInput{{"path1", Value(Decimal128(0))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseDecimalInput), false, BSONType::Bool);

    Document preciseZero{{"path1", Value(Decimal128("0.00"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(preciseZero), false, BSONType::Bool);

    Document negativeZero{{"path1", Value(Decimal128("-0.00"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeZero), false, BSONType::Bool);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalNaN), true, BSONType::Bool);

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalNegativeNaN), true, BSONType::Bool);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInfinity), true, BSONType::Bool);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertNumericToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "double"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document intInput{{"path1", Value(int{1})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(intInput), 1.0, BSONType::NumberDouble);

    Document longInput{{"path1", Value(0xf00000000ll)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(longInput), 64424509440.0, BSONType::NumberDouble);

    Document decimalInput{{"path1", Value(Decimal128("5.5"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInput), 5.5, BSONType::NumberDouble);

    Document boolFalse{{"path1", Value(false)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse), 0.0, BSONType::NumberDouble);

    Document boolTrue{{"path1", Value(true)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolTrue), 1.0, BSONType::NumberDouble);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    auto result = convertExp->evaluate(decimalNaN);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT(std::isnan(result.getDouble()));

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    result = convertExp->evaluate(decimalNegativeNaN);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT(std::isnan(result.getDouble()));

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    result = convertExp->evaluate(decimalInfinity);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_GT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    result = convertExp->evaluate(decimalNegativeInfinity);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_LT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));

    // Note that the least significant bits get lost, because the significand of a double is not
    // wide enough for the original long long value in its entirety.
    Document largeLongInput{{"path1", Value(0xf0000000000000fLL)}};
    result = convertExp->evaluate(largeLongInput);
    ASSERT_EQ(static_cast<long long>(result.getDouble()), 0xf00000000000000ll);

    // Again, some precision is lost in the conversion from Decimal128 to double.
    Document preciseDecimalInput{{"path1", Value(Decimal128("1.125000000000000000005"))}};
    result = convertExp->evaluate(preciseDecimalInput);
    ASSERT_EQ(result.getDouble(), 1.125);
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDecimalToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "double"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document overflowInput{{"path1", Decimal128("1e309")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document negativeOverflowInput{{"path1", Decimal128("-1e309")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDecimalToDoubleWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "double"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document overflowInput{{"path1", Decimal128("1e309")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    Document negativeOverflowInput{{"path1", Decimal128("-1e309")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertNumericToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document intInput{{"path1", Value(int{1})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(intInput), Decimal128(1), BSONType::NumberDecimal);

    Document longInput{{"path1", Value(0xf00000000ll)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(longInput),
                                   Decimal128(std::int64_t{0xf00000000LL}),
                                   BSONType::NumberDecimal);

    Document doubleInput{{"path1", Value(0.1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInput), Decimal128("0.1"), BSONType::NumberDecimal);

    Document boolFalse{{"path1", Value(false)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolFalse), Decimal128(0), BSONType::NumberDecimal);

    Document boolTrue{{"path1", Value(true)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolTrue), Decimal128(1), BSONType::NumberDecimal);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNaN), Decimal128::kPositiveNaN, BSONType::NumberDecimal);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInfinity),
                                   Decimal128::kPositiveInfinity,
                                   BSONType::NumberDecimal);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleNegativeInfinity),
                                   Decimal128::kNegativeInfinity,
                                   BSONType::NumberDecimal);

    // Unlike the similar conversion in ConvertNumericToDouble, there is more than enough precision
    // to store the exact orignal value in a Decimal128.
    Document largeLongInput{{"path1", Value(0xf0000000000000fLL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(largeLongInput), Value(0xf0000000000000fLL), BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertDoubleToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Value(1.0)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberInt);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Value(2.1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberInt);

    Document nonIntegerInput2{{"path1", Value(2.9)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberInt);

    Document nonIntegerInput3{{"path1", Value(-2.1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberInt);

    Document nonIntegerInput4{{"path1", Value(-2.9)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput4), -2, BSONType::NumberInt);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", Value(static_cast<double>(maxInt))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberInt);

    int minInt = std::numeric_limits<int>::lowest();
    Document minInput{{"path1", Value(static_cast<double>(minInt))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput), minInt, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDoubleToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    double overflowInt =
        std::nextafter(static_cast<double>(maxInt), std::numeric_limits<double>::max());
    Document overflowInput{{"path1", Value(overflowInt)}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    int minInt = std::numeric_limits<int>::lowest();
    double negativeOverflowInt =
        std::nextafter(static_cast<double>(minInt), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", Value(negativeOverflowInt)}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", Value(std::numeric_limits<double>::quiet_NaN())}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDoubleToIntWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    double overflowInt =
        std::nextafter(static_cast<double>(maxInt), std::numeric_limits<double>::max());
    Document overflowInput{{"path1", Value(overflowInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    int minInt = std::numeric_limits<int>::lowest();
    double negativeOverflowInt =
        std::nextafter(static_cast<double>(minInt), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", Value(negativeOverflowInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);

    Document nanInput{{"path1", Value(std::numeric_limits<double>::quiet_NaN())}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nanInput), "X"_sd, BSONType::String);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInfinity), "X"_sd, BSONType::String);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertDoubleToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Value(1.0)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberLong);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Value(2.1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberLong);

    Document nonIntegerInput2{{"path1", Value(2.9)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberLong);

    Document nonIntegerInput3{{"path1", Value(-2.1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberLong);

    Document nonIntegerInput4{{"path1", Value(-2.9)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4), -2, BSONType::NumberLong);

    // maxVal is the highest double value that will not overflow long long.
    double maxVal = std::nextafter(ExpressionConvert::kLongLongMaxPlusOneAsDouble, 0.0);
    Document maxInput{{"path1", Value(maxVal)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput), static_cast<long long>(maxVal), BSONType::NumberLong);

    // minVal is the lowest double value that will not overflow long long.
    double minVal = static_cast<double>(std::numeric_limits<long long>::lowest());
    Document minInput{{"path1", Value(minVal)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minInput), static_cast<long long>(minVal), BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDoubleToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    double overflowLong = ExpressionConvert::kLongLongMaxPlusOneAsDouble;
    Document overflowInput{{"path1", Value(overflowLong)}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    double minLong = static_cast<double>(std::numeric_limits<long long>::lowest());
    double negativeOverflowLong =
        std::nextafter(static_cast<double>(minLong), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", Value(negativeOverflowLong)}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", Value(std::numeric_limits<double>::quiet_NaN())}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDoubleToLongWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    double overflowLong = ExpressionConvert::kLongLongMaxPlusOneAsDouble;
    Document overflowInput{{"path1", Value(overflowLong)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    double minLong = static_cast<double>(std::numeric_limits<long long>::lowest());
    double negativeOverflowLong =
        std::nextafter(static_cast<double>(minLong), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", Value(negativeOverflowLong)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);

    Document nanInput{{"path1", Value(std::numeric_limits<double>::quiet_NaN())}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nanInput), "X"_sd, BSONType::String);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInfinity), "X"_sd, BSONType::String);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertDecimalToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Value(Decimal128("1.0"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberInt);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Value(Decimal128("2.1"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberInt);

    Document nonIntegerInput2{{"path1", Value(Decimal128("2.9"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberInt);

    Document nonIntegerInput3{{"path1", Value(Decimal128("-2.1"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberInt);

    Document nonIntegerInput4{{"path1", Value(Decimal128("-2.9"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberInt);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", Value(Decimal128(maxInt))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberInt);

    int minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", Value(Decimal128(minInt))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput), minInt, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDecimalToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", Decimal128(maxInt).add(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    int minInt = std::numeric_limits<int>::lowest();
    Document negativeOverflowInput{{"path1", Decimal128(minInt).subtract(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeNaNInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDecimalToIntWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", Decimal128(maxInt).add(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    int minInt = std::numeric_limits<int>::lowest();
    Document negativeOverflowInput{{"path1", Decimal128(minInt).subtract(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nanInput), "X"_sd, BSONType::String);

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeNaNInput), "X"_sd, BSONType::String);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInfinity), "X"_sd, BSONType::String);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertDecimalToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Value(Decimal128("1.0"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberLong);

    // Conversions to long should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Value(Decimal128("2.1"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberLong);

    Document nonIntegerInput2{{"path1", Value(Decimal128("2.9"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberLong);

    Document nonIntegerInput3{{"path1", Value(Decimal128("-2.1"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberLong);

    Document nonIntegerInput4{{"path1", Value(Decimal128("-2.9"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4), -2, BSONType::NumberLong);

    long long maxVal = std::numeric_limits<long long>::max();
    Document maxInput{{"path1", Value(Decimal128(std::int64_t{maxVal}))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxVal, BSONType::NumberLong);

    long long minVal = std::numeric_limits<long long>::min();
    Document minInput{{"path1", Value(Decimal128(std::int64_t{minVal}))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput), minVal, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDecimalToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    long long maxVal = std::numeric_limits<long long>::max();
    Document overflowInput{{"path1", Decimal128(std::int64_t{maxVal}).add(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    long long minVal = std::numeric_limits<long long>::lowest();
    Document negativeOverflowInput{
        {"path1", Decimal128(std::int64_t{minVal}).subtract(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeNaNInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsDecimalToLongWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    long long maxVal = std::numeric_limits<long long>::max();
    Document overflowInput{{"path1", Decimal128(std::int64_t{maxVal}).add(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    long long minVal = std::numeric_limits<long long>::lowest();
    Document negativeOverflowInput{
        {"path1", Decimal128(std::int64_t{minVal}).subtract(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nanInput), "X"_sd, BSONType::String);

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeNaNInput), "X"_sd, BSONType::String);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInfinity), "X"_sd, BSONType::String);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertIntToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Value(1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1LL, BSONType::NumberLong);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", Value(maxInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberLong);

    int minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", Value(minInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput), minInt, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertLongToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Value(1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberInt);

    long long maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", Value(maxInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberInt);

    long long minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", Value(minInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput), minInt, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsLongToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    long long maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", Value(maxInt + 1)}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    long long minInt = std::numeric_limits<int>::min();
    Document negativeOverflowInput{{"path1", Value(minInt - 1)}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsLongToIntWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    long long maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", Value(maxInt + 1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    long long minInt = std::numeric_limits<int>::min();
    Document negativeOverflowInput{{"path1", Value(minInt - 1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertBoolToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document boolFalse{{"path1", Value(false)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse), 0, BSONType::NumberInt);

    Document boolTrue{{"path1", Value(true)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolTrue), 1, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, ConvertBoolToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document boolFalse{{"path1", Value(false)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse), 0ll, BSONType::NumberLong);

    Document boolTrue{{"path1", Value(true)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolTrue), 1ll, BSONType::NumberLong);
}

}  // namespace ExpressionConvertTest

}  // namespace mongo
