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

#include "mongo/bson/oid.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

#define ASSERT_VALUE_CONTENTS_AND_TYPE(v, contents, type)  \
    do {                                                   \
        Value evaluatedResult = v;                         \
        ASSERT_VALUE_EQ(evaluatedResult, Value(contents)); \
        ASSERT_EQ(evaluatedResult.getType(), type);        \
    } while (false);

namespace ExpressionConvertTest {

static const long long kIntMax = std::numeric_limits<int>::max();
static const long long kIntMin = std::numeric_limits<int>::lowest();
static const long long kLongMax = std::numeric_limits<long long>::max();
static const double kLongMin = static_cast<double>(std::numeric_limits<long long>::lowest());
static const double kLongNegativeOverflow =
    std::nextafter(static_cast<double>(kLongMin), std::numeric_limits<double>::lowest());
static const Decimal128 kDoubleOverflow = Decimal128("1e309");
static const Decimal128 kDoubleNegativeOverflow = Decimal128("-1e309");

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

TEST_F(ExpressionConvertTest, UnsupportedConversionShouldThrowUnlessOnErrorProvided) {
    auto expCtx = getExpCtx();

    std::vector<std::pair<Value, std::string>> unsupportedConversions{
        // Except for the ones listed below, $convert supports all conversions between the supported
        // types: double, string, int, long, decimal, objectId, bool, int, and date.
        {Value(OID()), "double"},
        {Value(OID()), "int"},
        {Value(OID()), "long"},
        {Value(OID()), "decimal"},
        {Value(Date_t{}), "objectId"},
        {Value(Date_t{}), "int"},
        {Value(int{1}), "date"},
        {Value(true), "date"},

        // All conversions that involve any other type will fail, unless the target type is bool,
        // in which case the conversion results in a true value. Below is one conversion for each
        // of the unsupported types.
        {Value(1.0), "minKey"},
        {Value(1.0), "missing"},
        {Value(1.0), "object"},
        {Value(1.0), "array"},
        {Value(1.0), "binData"},
        {Value(1.0), "undefined"},
        {Value(1.0), "null"},
        {Value(1.0), "regex"},
        {Value(1.0), "dbPointer"},
        {Value(1.0), "javascript"},
        {Value(1.0), "symbol"},
        {Value(1.0), "javascriptWithScope"},
        {Value(1.0), "timestamp"},
        {Value(1.0), "maxKey"},
    };

    // Attempt all of the unsupported conversions listed above.
    for (auto conversion : unsupportedConversions) {
        auto inputValue = conversion.first;
        auto targetTypeName = conversion.second;

        auto spec = BSON("$convert" << BSON("input"
                                            << "$path1"
                                            << "to"
                                            << Value(targetTypeName)));

        Document input{{"path1", inputValue}};

        auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

        ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(input),
                                 AssertionException,
                                 [](const AssertionException& exception) {
                                     ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                     ASSERT_STRING_CONTAINS(exception.reason(),
                                                            "Unsupported conversion");
                                 });
    }

    // Attempt them again, this time with an "onError" value.
    for (auto conversion : unsupportedConversions) {
        auto inputValue = conversion.first;
        auto targetTypeName = conversion.second;

        auto spec = BSON("$convert" << BSON("input"
                                            << "$path1"
                                            << "to"
                                            << Value(targetTypeName)
                                            << "onError"
                                            << "X"));

        Document input{{"path1", inputValue}};

        auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

        ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(input), "X"_sd, BSONType::String);
    }
}

TEST_F(ExpressionConvertTest, ConvertNullishInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document nullInput{{"path1", BSONNULL}};
    Document undefinedInput{{"path1", BSONUndefined}};
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

    Document nullInput{{"path1", BSONNULL}};
    Document undefinedInput{{"path1", BSONUndefined}};
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

    Document nullInput{{"path1", BSONNULL}};
    Document undefinedInput{{"path1", BSONUndefined}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput), Value(BSONNULL));
}

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

    Document doubleInput{{"path1", 2.4}};
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

    Document trueBoolInput{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueBoolInput), true, BSONType::Bool);

    Document falseBoolInput{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseBoolInput), false, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, StringIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document stringInput{{"path1", "More cowbell"_sd}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(stringInput), "More cowbell"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ObjectIdIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "objectId"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("0123456789abcdef01234567")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(oidInput), OID("0123456789abcdef01234567"), BSONType::jstOID);
}

TEST_F(ExpressionConvertTest, DateIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "date"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t{}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(dateInput), Date_t{}, BSONType::Date);
}

TEST_F(ExpressionConvertTest, IntIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document intInput{{"path1", int{123}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(intInput), int{123}, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, LongIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document longInput{{"path1", 123LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(longInput), 123LL, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, DecimalIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document decimalInput{{"path1", Decimal128("2.4")}};
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

TEST_F(ExpressionConvertTest, ConvertDateToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    // All date inputs evaluate as true.
    Document dateInput{{"path1", Date_t{}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(dateInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertIntToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueIntInput{{"path1", int{1}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueIntInput), true, BSONType::Bool);

    Document falseIntInput{{"path1", int{0}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseIntInput), false, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertLongToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueLongInput{{"path1", -1LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueLongInput), true, BSONType::Bool);

    Document falseLongInput{{"path1", 0LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseLongInput), false, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertDoubleToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueDoubleInput{{"path1", 2.4}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueDoubleInput), true, BSONType::Bool);

    Document falseDoubleInput{{"path1", -0.0}};
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

    Document trueDecimalInput{{"path1", Decimal128(5)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueDecimalInput), true, BSONType::Bool);

    Document falseDecimalInput{{"path1", Decimal128(0)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseDecimalInput), false, BSONType::Bool);

    Document preciseZero{{"path1", Decimal128("0.00")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(preciseZero), false, BSONType::Bool);

    Document negativeZero{{"path1", Decimal128("-0.00")}};
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

TEST_F(ExpressionConvertTest, ConvertStringToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document stringInput{{"path1", "str"_sd}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(stringInput), true, BSONType::Bool);

    Document emptyStringInput{{"path1", ""_sd}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(emptyStringInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertObjectIdToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("59E8A8D8FEDCBA9876543210")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(oidInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertMinKeyToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document minKeyInput{{"path1", MINKEY}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minKeyInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertObjectToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document objectInput{{"path1", Document{{"foo", 1}}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(objectInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertArrayToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document arrayInput{{"path1", BSON_ARRAY(1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(arrayInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertBinDataToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    char data[] = "(^_^)";
    Document binInput{{"path1", BSONBinData(data, sizeof(data), BinDataType::BinDataGeneral)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(binInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertRegexToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document regexInput{{"path1", BSONRegEx("ab*a"_sd)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(regexInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertDBRefToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document refInput{{"path1", BSONDBRef("db.coll"_sd, OID("aaaaaaaaaaaaaaaaaaaaaaaa"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(refInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertCodeToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document codeInput{{"path1", BSONCode("print('Hello world!');"_sd)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(codeInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertSymbolToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document symbolInput{{"path1", BSONSymbol("print"_sd)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(symbolInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertCodeWScopeToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document codeWScopeInput{
        {"path1", BSONCodeWScope("print('Hello again, world!')"_sd, BSONObj())}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(codeWScopeInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertTimestampToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document timestampInput{{"path1", Timestamp()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(timestampInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertMaxKeyToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "bool"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document maxKeyInput{{"path1", MAXKEY}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxKeyInput), true, BSONType::Bool);
}

TEST_F(ExpressionConvertTest, ConvertNumericToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "double"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document intInput{{"path1", int{1}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(intInput), 1.0, BSONType::NumberDouble);

    Document longInput{{"path1", 0xf00000000LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(longInput), 64424509440.0, BSONType::NumberDouble);

    Document decimalInput{{"path1", Decimal128("5.5")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInput), 5.5, BSONType::NumberDouble);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse), 0.0, BSONType::NumberDouble);

    Document boolTrue{{"path1", true}};
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
    Document largeLongInput{{"path1", 0xf0000000000000fLL}};
    result = convertExp->evaluate(largeLongInput);
    ASSERT_EQ(static_cast<long long>(result.getDouble()), 0xf00000000000000LL);

    // Again, some precision is lost in the conversion from Decimal128 to double.
    Document preciseDecimalInput{{"path1", Decimal128("1.125000000000000000005")}};
    result = convertExp->evaluate(preciseDecimalInput);
    ASSERT_EQ(result.getDouble(), 1.125);
}

TEST_F(ExpressionConvertTest, ConvertDateToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "double"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(123)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(dateInput), 123.0, BSONType::NumberDouble);

    // Note that the least significant bits get lost, because the significand of a double is not
    // wide enough for the original 64-bit Date_t value in its entirety.
    Document largeDateInput{{"path1", Date_t::fromMillisSinceEpoch(0xf0000000000000fLL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(largeDateInput), 0xf00000000000000LL, BSONType::NumberDouble);
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

    Document intInput{{"path1", int{1}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(intInput), Decimal128(1), BSONType::NumberDecimal);

    Document longInput{{"path1", 0xf00000000LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(longInput),
                                   Decimal128(std::int64_t{0xf00000000LL}),
                                   BSONType::NumberDecimal);

    Document doubleInput{{"path1", 0.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInput), Decimal128("0.1"), BSONType::NumberDecimal);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolFalse), Decimal128(0), BSONType::NumberDecimal);

    Document boolTrue{{"path1", true}};
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
    Document largeLongInput{{"path1", 0xf0000000000000fLL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(largeLongInput), 0xf0000000000000fLL, BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertDateToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(123)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(dateInput), Decimal128(123), BSONType::NumberDecimal);

    Document largeDateInput{{"path1", Date_t::fromMillisSinceEpoch(0xf0000000000000fLL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(largeDateInput), 0xf0000000000000fLL, BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertDoubleToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberInt);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", 2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberInt);

    Document nonIntegerInput2{{"path1", 2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberInt);

    Document nonIntegerInput3{{"path1", -2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberInt);

    Document nonIntegerInput4{{"path1", -2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput4), -2, BSONType::NumberInt);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", static_cast<double>(maxInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberInt);

    int minInt = std::numeric_limits<int>::lowest();
    Document minInput{{"path1", static_cast<double>(minInt)}};
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
    Document overflowInput{{"path1", overflowInt}};
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
    Document negativeOverflowInput{{"path1", negativeOverflowInt}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
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
    Document overflowInput{{"path1", overflowInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    int minInt = std::numeric_limits<int>::lowest();
    double negativeOverflowInt =
        std::nextafter(static_cast<double>(minInt), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", negativeOverflowInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
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

    Document simpleInput{{"path1", 1.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberLong);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", 2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberLong);

    Document nonIntegerInput2{{"path1", 2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberLong);

    Document nonIntegerInput3{{"path1", -2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberLong);

    Document nonIntegerInput4{{"path1", -2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4), -2, BSONType::NumberLong);

    // maxVal is the highest double value that will not overflow long long.
    double maxVal = std::nextafter(ExpressionConvert::kLongLongMaxPlusOneAsDouble, 0.0);
    Document maxInput{{"path1", maxVal}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput), static_cast<long long>(maxVal), BSONType::NumberLong);

    // minVal is the lowest double value that will not overflow long long.
    double minVal = static_cast<double>(std::numeric_limits<long long>::lowest());
    Document minInput{{"path1", minVal}};
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
    Document overflowInput{{"path1", overflowLong}};
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
    Document negativeOverflowInput{{"path1", negativeOverflowLong}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
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
    Document overflowInput{{"path1", overflowLong}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    double minLong = static_cast<double>(std::numeric_limits<long long>::lowest());
    double negativeOverflowLong =
        std::nextafter(static_cast<double>(minLong), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", negativeOverflowLong}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput), "X"_sd, BSONType::String);

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
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

    Document simpleInput{{"path1", Decimal128("1.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberInt);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Decimal128("2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberInt);

    Document nonIntegerInput2{{"path1", Decimal128("2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberInt);

    Document nonIntegerInput3{{"path1", Decimal128("-2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberInt);

    Document nonIntegerInput4{{"path1", Decimal128("-2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberInt);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", Decimal128(maxInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberInt);

    int minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", Decimal128(minInt)}};
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

    Document simpleInput{{"path1", Decimal128("1.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberLong);

    // Conversions to long should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Decimal128("2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput1), 2, BSONType::NumberLong);

    Document nonIntegerInput2{{"path1", Decimal128("2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nonIntegerInput2), 2, BSONType::NumberLong);

    Document nonIntegerInput3{{"path1", Decimal128("-2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3), -2, BSONType::NumberLong);

    Document nonIntegerInput4{{"path1", Decimal128("-2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4), -2, BSONType::NumberLong);

    long long maxVal = std::numeric_limits<long long>::max();
    Document maxInput{{"path1", Decimal128(std::int64_t{maxVal})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxVal, BSONType::NumberLong);

    long long minVal = std::numeric_limits<long long>::min();
    Document minInput{{"path1", Decimal128(std::int64_t{minVal})}};
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

TEST_F(ExpressionConvertTest, ConvertDateToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(123LL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(dateInput), 123LL, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertIntToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1LL, BSONType::NumberLong);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", maxInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberLong);

    int minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", minInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput), minInt, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertLongToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "int"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput), 1, BSONType::NumberInt);

    long long maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", maxInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput), maxInt, BSONType::NumberInt);

    long long minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", minInt}};
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
    Document overflowInput{{"path1", maxInt + 1}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    long long minInt = std::numeric_limits<int>::min();
    Document negativeOverflowInput{{"path1", minInt - 1}};
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
    Document overflowInput{{"path1", maxInt + 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(overflowInput), "X"_sd, BSONType::String);

    long long minInt = std::numeric_limits<int>::min();
    Document negativeOverflowInput{{"path1", minInt - 1}};
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

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse), 0, BSONType::NumberInt);

    Document boolTrue{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolTrue), 1, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, ConvertBoolToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "long"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse), 0LL, BSONType::NumberLong);

    Document boolTrue{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolTrue), 1LL, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertNumberToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "date"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document longInput{{"path1", 0LL}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(longInput).getDate()),
              "1970-01-01T00:00:00.000Z");

    Document doubleInput{{"path1", 431568000000.0}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(doubleInput).getDate()),
              "1983-09-05T00:00:00.000Z");

    Document doubleInputWithFraction{{"path1", 431568000000.987}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(doubleInputWithFraction).getDate()),
              "1983-09-05T00:00:00.000Z");

    Document decimalInput{{"path1", Decimal128("872835240000")}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(decimalInput).getDate()),
              "1997-08-29T06:14:00.000Z");

    Document decimalInputWithFraction{{"path1", Decimal128("872835240000.987")}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(decimalInputWithFraction).getDate()),
              "1997-08-29T06:14:00.000Z");
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsNumberToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "date"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document doubleOverflowInput{{"path1", 1.0e100}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document doubleNegativeOverflowInput{{"path1", -1.0e100}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNaN),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert NaN value to integer type");
                             });

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });

    Document decimalOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document decimalNegativeOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeOverflowInput),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNaN),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert NaN value to integer type");
                             });

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeNaN),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert NaN value to integer type");
                             });

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeInfinity),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });
}

TEST_F(ExpressionConvertTest, ConvertOutOfBoundsNumberToDateWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "date"
                                        << "onError"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    // Int is explicitly disallowed for date conversions. Clients must use 64-bit long instead.
    Document intInput{{"path1", int{0}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(intInput), "X"_sd, BSONType::String);

    Document doubleOverflowInput{{"path1", 1.0e100}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleOverflowInput), "X"_sd, BSONType::String);

    Document doubleNegativeOverflowInput{{"path1", -1.0e100}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeOverflowInput), "X"_sd, BSONType::String);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleNaN), "X"_sd, BSONType::String);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInfinity), "X"_sd, BSONType::String);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity), "X"_sd, BSONType::String);

    Document decimalOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalOverflowInput), "X"_sd, BSONType::String);

    Document decimalNegativeOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeOverflowInput), "X"_sd, BSONType::String);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalNaN), "X"_sd, BSONType::String);

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeNaN), "X"_sd, BSONType::String);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInfinity), "X"_sd, BSONType::String);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity), "X"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertObjectIdToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "date"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("59E8A8D8FEDCBA9876543210")}};

    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(oidInput).getDate()),
              "2017-10-19T13:30:00.000Z");
}

TEST_F(ExpressionConvertTest, ConvertObjectIdToDateFuture) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "date"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("8071be82122d3312074f0300")}};

    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(oidInput).getDate()),
              "2038-04-15T09:53:06.000Z");
}

TEST_F(ExpressionConvertTest, ConvertStringToInt) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'int'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5, BSONType::NumberInt);

    spec = fromjson("{$convert: {input: '" + std::to_string(kIntMax) + "', to: 'int'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), kIntMax, BSONType::NumberInt);
}

TEST_F(ExpressionConvertTest, ConvertStringToIntOverflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '" + std::to_string(kIntMax + 1) + "', to: 'int'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
        });

    spec = fromjson("{$convert: {input: '" + std::to_string(kIntMin - 1) + "', to: 'int'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToIntOverflowWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec = fromjson("{$convert: {input: '" + std::to_string(kIntMax + 1) +
                         "', to: 'int', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '" + std::to_string(kIntMin - 1) +
                    "', to: 'int', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertStringToLong) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'long'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5LL, BSONType::NumberLong);

    spec = fromjson("{$convert: {input: '" + std::to_string(kLongMax) + "', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), kLongMax, BSONType::NumberLong);
}

TEST_F(ExpressionConvertTest, ConvertStringToLongOverflow) {
    auto expCtx = getExpCtx();
    auto longMaxPlusOneAsString = std::to_string(ExpressionConvert::kLongLongMaxPlusOneAsDouble);
    // Remove digits after the decimal to avoid parse failure.
    longMaxPlusOneAsString = longMaxPlusOneAsString.substr(0, longMaxPlusOneAsString.find('.'));

    auto spec = fromjson("{$convert: {input: '" + longMaxPlusOneAsString + "', to: 'long'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
        });

    auto longMinMinusOneAsString = std::to_string(kLongNegativeOverflow);
    longMinMinusOneAsString = longMinMinusOneAsString.substr(0, longMinMinusOneAsString.find('.'));

    spec = fromjson("{$convert: {input: '" + longMinMinusOneAsString + "', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToLongFailsForFloats) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5.5', to: 'long'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Bad digit \".\"");
        });

    spec = fromjson("{$convert: {input: '5.0', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Bad digit \".\"");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToLongWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;
    auto longMaxPlusOneAsString = std::to_string(ExpressionConvert::kLongLongMaxPlusOneAsDouble);
    // Remove digits after the decimal to avoid parse failure.
    longMaxPlusOneAsString = longMaxPlusOneAsString.substr(0, longMaxPlusOneAsString.find('.'));

    auto spec = fromjson("{$convert: {input: '" + longMaxPlusOneAsString +
                         "', to: 'long', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    auto longMinMinusOneAsString = std::to_string(kLongNegativeOverflow);
    longMinMinusOneAsString = longMinMinusOneAsString.substr(0, longMinMinusOneAsString.find('.'));

    spec = fromjson("{$convert: {input: '" + longMinMinusOneAsString + "', to: 'long', onError: '" +
                    onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '5.5', to: 'long', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '5.0', to: 'long', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertStringToDouble) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5.0, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '5.5', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5.5, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '.5', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 0.5, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '+5', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5.0, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '+5.0e42', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5.0e42, BSONType::NumberDouble);
}

TEST_F(ExpressionConvertTest, ConvertStringToDoubleWithPrecisionLoss) {
    auto expCtx = getExpCtx();

    // Note that the least significant bits get lost, because the significand of a double is not
    // wide enough for the given input string in its entirety.
    auto spec = fromjson("{$convert: {input: '10000000000000000001', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 1e19, BSONType::NumberDouble);

    // Again, some precision is lost in the conversion to double.
    spec = fromjson("{$convert: {input: '1.125000000000000000005', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 1.125, BSONType::NumberDouble);
}

TEST_F(ExpressionConvertTest, ConvertStringToDoubleFailsForInvalidFloats) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '.5.', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Did not consume whole number");
        });

    spec = fromjson("{$convert: {input: '5.5f', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Did not consume whole number");
        });
}

TEST_F(ExpressionConvertTest, ConvertInfinityStringsToDouble) {
    auto expCtx = getExpCtx();
    auto infValue = std::numeric_limits<double>::infinity();

    auto spec = fromjson("{$convert: {input: 'Infinity', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: 'INF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: 'infinity', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '+InFiNiTy', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '-Infinity', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), -infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '-INF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), -infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '-InFiNiTy', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), -infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '-inf', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), -infValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: '-infinity', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), -infValue, BSONType::NumberDouble);
}

TEST_F(ExpressionConvertTest, ConvertZeroStringsToDouble) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '-0', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDouble);
    ASSERT_TRUE(std::signbit(result.getDouble()));

    spec = fromjson("{$convert: {input: '-0.0', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDouble);
    ASSERT_TRUE(std::signbit(result.getDouble()));

    spec = fromjson("{$convert: {input: '+0', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDouble);
    ASSERT_FALSE(std::signbit(result.getDouble()));

    spec = fromjson("{$convert: {input: '+0.0', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDouble);
    ASSERT_FALSE(std::signbit(result.getDouble()));
}

TEST_F(ExpressionConvertTest, ConvertNanStringsToDouble) {
    auto expCtx = getExpCtx();
    auto nanValue = std::numeric_limits<double>::quiet_NaN();

    auto spec = fromjson("{$convert: {input: 'nan', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto result = convertExp->evaluate({});
    ASSERT_TRUE(std::isnan(result.getDouble()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), nanValue, BSONType::NumberDouble);

    spec = fromjson("{$convert: {input: 'Nan', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_TRUE(std::isnan(result.getDouble()));

    spec = fromjson("{$convert: {input: 'NaN', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_TRUE(std::isnan(result.getDouble()));

    spec = fromjson("{$convert: {input: '-NAN', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_TRUE(std::isnan(result.getDouble()));

    spec = fromjson("{$convert: {input: '+NaN', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_TRUE(std::isnan(result.getDouble()));
}

TEST_F(ExpressionConvertTest, ConvertStringToDoubleOverflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '" + kDoubleOverflow.toString() + "', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Out of range");
        });

    spec =
        fromjson("{$convert: {input: '" + kDoubleNegativeOverflow.toString() + "', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Out of range");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToDoubleUnderflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '1E-1000', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "Failed to parse number '1E-1000' in $convert with no onError value: Out of range");
        });

    spec = fromjson("{$convert: {input: '-1E-1000', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Failed to parse number '-1E-1000' in $convert with no onError "
                                   "value: Out of range");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToDoubleWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec = fromjson("{$convert: {input: '" + kDoubleOverflow.toString() +
                         "', to: 'double', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '" + kDoubleNegativeOverflow.toString() +
                    "', to: 'double', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '.5.', to: 'double', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '5.5f', to: 'double', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertStringToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), 5, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '2.02', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), Decimal128("2.02"), BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '2.02E200', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), Decimal128("2.02E200"), BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '" + Decimal128::kLargestPositive.toString() +
                    "', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), Decimal128::kLargestPositive, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '" + Decimal128::kLargestNegative.toString() +
                    "', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), Decimal128::kLargestNegative, BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertInfinityStringsToDecimal) {
    auto expCtx = getExpCtx();
    auto infValue = Decimal128::kPositiveInfinity;
    auto negInfValue = Decimal128::kNegativeInfinity;

    auto spec = fromjson("{$convert: {input: 'Infinity', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: 'INF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: 'infinity', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '+InFiNiTy', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), infValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-Infinity', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negInfValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-INF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negInfValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-InFiNiTy', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negInfValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-inf', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negInfValue, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-infinity', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negInfValue, BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertNanStringsToDecimal) {
    auto expCtx = getExpCtx();
    auto positiveNan = Decimal128::kPositiveNaN;
    auto negativeNan = Decimal128::kNegativeNaN;

    auto spec = fromjson("{$convert: {input: 'nan', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), positiveNan, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: 'Nan', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), positiveNan, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: 'NaN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), positiveNan, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '+NaN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), positiveNan, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-NAN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negativeNan, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-nan', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negativeNan, BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '-NaN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), negativeNan, BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertZeroStringsToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '-0', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_TRUE(result.getDecimal().isNegative());

    spec = fromjson("{$convert: {input: '-0.0', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_TRUE(result.getDecimal().isNegative());

    spec = fromjson("{$convert: {input: '+0', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_FALSE(result.getDecimal().isNegative());

    spec = fromjson("{$convert: {input: '+0.0', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::NumberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_FALSE(result.getDecimal().isNegative());
}

TEST_F(ExpressionConvertTest, ConvertStringToDecimalOverflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '1E6145', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Conversion from string to decimal would overflow");
        });

    spec = fromjson("{$convert: {input: '-1E6145', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Conversion from string to decimal would overflow");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToDecimalUnderflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '1E-6178', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Conversion from string to decimal would underflow");
        });

    spec = fromjson("{$convert: {input: '-1E-6177', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Conversion from string to decimal would underflow");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToDecimalWithPrecisionLoss) {
    auto expCtx = getExpCtx();

    auto spec =
        fromjson("{$convert: {input: '10000000000000000000000000000000001', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), Decimal128("1e34"), BSONType::NumberDecimal);

    spec = fromjson("{$convert: {input: '1.1250000000000000000000000000000001', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), Decimal128("1.125"), BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertTest, ConvertStringToDecimalWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec =
        fromjson("{$convert: {input: '1E6145', to: 'decimal', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec =
        fromjson("{$convert: {input: '-1E-6177', to: 'decimal', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertStringToNumberFailsForHexStrings) {
    auto expCtx = getExpCtx();
    auto invalidHexFailure = [](const AssertionException& exception) {
        ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
        ASSERT_STRING_CONTAINS(exception.reason(), "Illegal hexadecimal input in $convert");
    };

    auto spec = fromjson("{$convert: {input: '0xFF', to: 'int'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0xFF', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0xFF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0xFF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0x00', to: 'int'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0x00', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: 'FF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Did not consume whole number");
        });

    spec = fromjson("{$convert: {input: 'FF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Failed to parse string to decimal");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToOID) {
    auto expCtx = getExpCtx();
    auto oid = OID::gen();

    auto spec = fromjson("{$convert: {input: '" + oid.toString() + "', to: 'objectId'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), oid, BSONType::jstOID);

    spec = fromjson("{$convert: {input: '123456789abcdef123456789', to: 'objectId'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}), OID("123456789abcdef123456789"), BSONType::jstOID);
}

TEST_F(ExpressionConvertTest, ConvertStringToOIDFailsForInvalidHexStrings) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: 'InvalidHexButSizeCorrect', to: 'objectId'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Invalid character found in hex string");
        });

    spec = fromjson("{$convert: {input: 'InvalidSize', to: 'objectId'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Invalid string length for parsing to OID");
        });

    spec = fromjson("{$convert: {input: '0x123456789abcdef123456789', to: 'objectId'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}), AssertionException, [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(), "Invalid string length for parsing to OID");
        });
}

TEST_F(ExpressionConvertTest, ConvertStringToOIDWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec =
        fromjson("{$convert: {input: 'InvalidHexButSizeCorrect', to: 'objectId', onError: '" +
                 onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: 'InvalidSize', to: 'objectId', onError: '" + onErrorValue +
                    "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);

    spec = fromjson("{$convert: {input: '0x123456789abcdef123456789', to: 'objectId', onError: '" +
                    onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}), onErrorValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertStringToDateRejectsUnparsableString) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '60.Monday1770/06:59', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);

    spec = fromjson("{$convert: {input: 'Definitely not a date', to: 'date'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionConvertTest, ConvertStringToDateRejectsTimezoneNameInString) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '2017-07-13T10:02:57 Europe/London', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);

    spec = fromjson("{$convert: {input: 'July 4, 2017 Europe/London', to: 'date'}}");
    convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionConvertTest, ConvertStringToDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("2017-07-06T12:35:37Z"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-06T12:35:37.000Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-07-06T12:35:37.513Z"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-06T12:35:37.513Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-07-06"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-06T00:00:00.000Z", result.toString());
}

TEST_F(ExpressionConvertTest, ConvertStringWithTimezoneToDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("2017-07-14T12:02:44.771 GMT+02:00"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-14T10:02:44.771Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-07-14T12:02:44.771 A"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-14T11:02:44.771Z", result.toString());
}

TEST_F(ExpressionConvertTest, ConvertVerbalStringToDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("July 4th, 2017"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-04T00:00:00.000Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("July 4th, 2017 12pm"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-04T12:00:00.000Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-Jul-04 noon"_sd)}});
    ASSERT_EQ(result.getType(), BSONType::Date);
    ASSERT_EQ("2017-07-04T12:00:00.000Z", result.toString());
}

TEST_F(ExpressionConvertTest, ConvertStringToDateWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "(-_-)"_sd;

    auto spec =
        fromjson("{$convert: {input: '$path1', to: 'date', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("Not a date"_sd)}});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onErrorValue, BSONType::String);

    result = convertExp->evaluate({{"path1", Value("60.Monday1770/06:59"_sd)}});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onErrorValue, BSONType::String);

    result = convertExp->evaluate({{"path1", Value("2017-07-13T10:02:57 Europe/London"_sd)}});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onErrorValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, ConvertStringToDateWithOnNull) {
    auto expCtx = getExpCtx();
    const auto onNullValue = "(-_-)"_sd;

    auto spec =
        fromjson("{$convert: {input: '$path1', to: 'date', onNull: '" + onNullValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onNullValue, BSONType::String);

    result = convertExp->evaluate({{"path1", Value(BSONNULL)}});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onNullValue, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", 0.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(zeroInput), "0"_sd, BSONType::String);

    Document negativeZeroInput{{"path1", -0.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeZeroInput), "-0"_sd, BSONType::String);

    Document positiveIntegerInput{{"path1", 1337.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveIntegerInput), "1337"_sd, BSONType::String);

    Document negativeIntegerInput{{"path1", -1337.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeIntegerInput), "-1337"_sd, BSONType::String);

    Document positiveFractionalInput{{"path1", 0.1337}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveFractionalInput), "0.1337"_sd, BSONType::String);

    Document negativeFractionalInput{{"path1", -0.1337}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeFractionalInput), "-0.1337"_sd, BSONType::String);

    Document positiveLargeInput{{"path1", 1.3e37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveLargeInput), "1.3e+37"_sd, BSONType::String);

    Document negativeLargeInput{{"path1", -1.3e37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeLargeInput), "-1.3e+37"_sd, BSONType::String);

    Document positiveTinyInput{{"path1", 1.3e-37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveTinyInput), "1.3e-37"_sd, BSONType::String);

    Document negativeTinyInput{{"path1", -1.3e-37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeTinyInput), "-1.3e-37"_sd, BSONType::String);

    Document infinityInput{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(infinityInput), "Infinity"_sd, BSONType::String);

    Document negativeInfinityInput{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeInfinityInput), "-Infinity"_sd, BSONType::String);

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nanInput), "NaN"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatObjectId) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", OID("000000000000000000000000")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(zeroInput), "000000000000000000000000"_sd, BSONType::String);

    Document simpleInput{{"path1", OID("0123456789abcdef01234567")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput), "0123456789abcdef01234567"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document trueInput{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(trueInput), "true"_sd, BSONType::String);

    Document falseInput{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(falseInput), "false"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document epochInput{{"path1", Date_t::fromMillisSinceEpoch(0LL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(epochInput), "1970-01-01T00:00:00.000Z"_sd, BSONType::String);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(872835240000)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(dateInput), "1997-08-29T06:14:00.000Z"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", int{0}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(zeroInput), "0"_sd, BSONType::String);

    Document positiveInput{{"path1", int{1337}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveInput), "1337"_sd, BSONType::String);

    Document negativeInput{{"path1", int{-1337}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeInput), "-1337"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", 0LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(zeroInput), "0"_sd, BSONType::String);

    Document positiveInput{{"path1", 1337133713371337LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveInput), "1337133713371337"_sd, BSONType::String);

    Document negativeInput{{"path1", -1337133713371337LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeInput), "-1337133713371337"_sd, BSONType::String);
}

TEST_F(ExpressionConvertTest, FormatDecimal) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input"
                                        << "$path1"
                                        << "to"
                                        << "string"));
    auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", Decimal128("0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(zeroInput), "0"_sd, BSONType::String);

    Document negativeZeroInput{{"path1", Decimal128("-0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeZeroInput), "-0"_sd, BSONType::String);

    Document preciseZeroInput{{"path1", Decimal128("0.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(preciseZeroInput), "0.0"_sd, BSONType::String);

    Document negativePreciseZeroInput{{"path1", Decimal128("-0.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativePreciseZeroInput), "-0.0"_sd, BSONType::String);

    Document extraPreciseZeroInput{{"path1", Decimal128("0.0000")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(extraPreciseZeroInput), "0.0000"_sd, BSONType::String);

    Document positiveIntegerInput{{"path1", Decimal128("1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveIntegerInput), "1337"_sd, BSONType::String);

    Document largeIntegerInput{{"path1", Decimal128("13370000000000000000000000000000000")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(largeIntegerInput),
                                   "1.337000000000000000000000000000000E+34"_sd,
                                   BSONType::String);

    Document negativeIntegerInput{{"path1", Decimal128("-1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeIntegerInput), "-1337"_sd, BSONType::String);

    Document positiveFractionalInput{{"path1", Decimal128("0.1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveFractionalInput), "0.1337"_sd, BSONType::String);

    Document positivePreciseFractionalInput{{"path1", Decimal128("0.133700")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positivePreciseFractionalInput), "0.133700"_sd, BSONType::String);

    Document negativeFractionalInput{{"path1", Decimal128("-0.1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeFractionalInput), "-0.1337"_sd, BSONType::String);

    Document negativePreciseFractionalInput{{"path1", Decimal128("-0.133700")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativePreciseFractionalInput), "-0.133700"_sd, BSONType::String);

    Document positiveLargeInput{{"path1", Decimal128("1.3e37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveLargeInput), "1.3E+37"_sd, BSONType::String);

    Document negativeLargeInput{{"path1", Decimal128("-1.3e37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeLargeInput), "-1.3E+37"_sd, BSONType::String);

    Document positiveTinyInput{{"path1", Decimal128("1.3e-37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveTinyInput), "1.3E-37"_sd, BSONType::String);

    Document negativeTinyInput{{"path1", Decimal128("-1.3e-37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeTinyInput), "-1.3E-37"_sd, BSONType::String);

    Document infinityInput{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(infinityInput), "Infinity"_sd, BSONType::String);

    Document negativeInfinityInput{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeInfinityInput), "-Infinity"_sd, BSONType::String);

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(nanInput), "NaN"_sd, BSONType::String);

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeNaNInput), "NaN"_sd, BSONType::String);
}

}  // namespace ExpressionConvertTest

namespace ExpressionConvertShortcutsTest {

using ExpressionConvertShortcutsTest = AggregationContextFixture;

TEST_F(ExpressionConvertShortcutsTest, RejectsMoreThanOneInput) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << BSON_ARRAY(1 << 3));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toLong" << BSON_ARRAY(1 << 3));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toDouble" << BSON_ARRAY(1 << 3));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
}

TEST_F(ExpressionConvertShortcutsTest, RejectsZeroInputs) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << BSONArray());
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toLong" << BSONArray());
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toDouble" << BSONArray());
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
}

TEST_F(ExpressionConvertShortcutsTest, AcceptsSingleArgumentInArrayOrByItself) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt"
                        << "1");
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));

    spec = BSON("$toInt" << BSON_ARRAY("1"));
    convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));

    spec = BSON("$toInt" << BSON_ARRAY(BSON_ARRAY("1")));
    convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convert->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToInts) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt"
                        << "1");
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(1), BSONType::NumberInt);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToLongs) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toLong"
                        << "1");
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(1), BSONType::NumberLong);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToDoubles) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDouble"
                        << "1");
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(1), BSONType::NumberDouble);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToDecimals) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDecimal"
                        << "1");
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(1), BSONType::NumberDecimal);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToDates) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDate" << 0LL);
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}), Value(Date_t::fromMillisSinceEpoch(0)), BSONType::Date);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToObjectIds) {
    auto expCtx = getExpCtx();

    const auto hexString = "deadbeefdeadbeefdeadbeef"_sd;
    BSONObj spec = BSON("$toObjectId" << hexString);
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}), Value(OID::createFromString(hexString)), BSONType::jstOID);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToString) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toString" << 1);
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value("1"_sd), BSONType::String);
}

TEST_F(ExpressionConvertShortcutsTest, ConvertsToBool) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toBool" << 1);
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(true), BSONType::Bool);
}

TEST_F(ExpressionConvertShortcutsTest, ReturnsNullOnNullishInput) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toBool" << BSONNULL);
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(BSONNULL), BSONType::jstNULL);

    spec = BSON("$toInt"
                << "$missing");
    convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}), Value(BSONNULL), BSONType::jstNULL);
}

TEST_F(ExpressionConvertShortcutsTest, ThrowsOnConversionFailure) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt"
                        << "not an int");
    auto convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_THROWS_CODE(convert->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);

    spec = BSON("$toObjectId"
                << "not all hex values");
    convert = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_THROWS_CODE(convert->evaluate({}), AssertionException, ErrorCodes::ConversionFailure);
}

}  // namespace ExpressionConvertShortcutsTest
}  // namespace mongo
