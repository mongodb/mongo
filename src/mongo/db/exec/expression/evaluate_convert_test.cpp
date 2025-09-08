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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

#define ASSERT_VALUE_CONTENTS_AND_TYPE(v, contents, type)  \
    do {                                                   \
        Value evaluatedResult = v;                         \
        ASSERT_VALUE_EQ(evaluatedResult, Value(contents)); \
        ASSERT_EQ(evaluatedResult.getType(), type);        \
    } while (false);

namespace evaluate_convert_test {

static const long long kIntMax = std::numeric_limits<int>::max();
static const long long kIntMin = std::numeric_limits<int>::lowest();
static const long long kLongMax = std::numeric_limits<long long>::max();
static const double kLongMin = static_cast<double>(std::numeric_limits<long long>::lowest());
static const double kLongNegativeOverflow =
    std::nextafter(static_cast<double>(kLongMin), std::numeric_limits<double>::lowest());
static const Decimal128 kDoubleOverflow = Decimal128("1e309");
static const Decimal128 kDoubleNegativeOverflow = Decimal128("-1e309");

using EvaluateConvertTest = AggregationContextFixture;

TEST_F(EvaluateConvertTest, ConvertToBinDataWithNonNumericSubtypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << BSON("type" << "binData"
                                                               << "subtype"
                                                               << "newUUID")
                                                << "format" << toStringData(BinDataFormat::kUuid)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path1", "abc"_sd}};
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(input, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), 4341108);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "$convert's 'subtype' argument must be a number, but is string");
        });
}

TEST_F(EvaluateConvertTest, ConvertToBinDataWithInvalidUtf8Fails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "binData"
                                                << "format" << toStringData(BinDataFormat::kUtf8)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path1", "\xE2\x82"_sd}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(input, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Invalid UTF-8");
                             });
}

TEST_F(EvaluateConvertTest, ConvertToStringWithInvalidUtf8BinDataFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"
                                                << "format" << toStringData(BinDataFormat::kUtf8)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto invalidUtf8 = "\xE2\x82";
    BSONBinData inputBinData{
        invalidUtf8, static_cast<int>(std::strlen(invalidUtf8)), BinDataGeneral};
    Document input{{"path1", inputBinData}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(input, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "BinData does not represent a valid UTF-8 string");
                             });
}

TEST_F(EvaluateConvertTest, ConvertToBinDataWithOutOfBoundsSubtypeFails) {
    auto expCtx = getExpCtx();

    auto assertFailsWithOutOfBoundsSubtype = [&](int subtypeValue) {
        auto spec =
            BSON("$convert" << BSON("input" << "$path1"
                                            << "to"
                                            << BSON("type" << "binData"
                                                           << "subtype" << subtypeValue)
                                            << "format" << toStringData(BinDataFormat::kBase64)));
        auto convertExp =
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document input{{"path1", "abc"_sd}};
        ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(input, &expCtx->variables),
                                 AssertionException,
                                 [](const AssertionException& exception) {
                                     ASSERT_EQ(exception.code(), 4341107);
                                     ASSERT_STRING_CONTAINS(
                                         exception.reason(),
                                         "In $convert, numeric value for 'subtype' does not "
                                         "correspond to a BinData type");
                                 });
    };

    assertFailsWithOutOfBoundsSubtype(900);
    assertFailsWithOutOfBoundsSubtype(-1);
}

TEST_F(EvaluateConvertTest, ConvertToBinDataWithNumericNonIntegerSubtypeFails) {
    auto expCtx = getExpCtx();

    auto spec =
        BSON("$convert" << BSON("input" << "$path1"
                                        << "to"
                                        << BSON("type" << "binData"
                                                       << "subtype" << 1.5)
                                        << "format" << toStringData(BinDataFormat::kBase64)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path1", "abc"_sd}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(input, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), 4341106);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "In $convert, numeric 'subtype' argument is not an integer");
                             });
}

TEST_F(EvaluateConvertTest, InvalidTypeNameFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "dinosaur"
                                                << "onError" << 0));

    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document(), &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::BadValue);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Unknown type name");
                             });
}

TEST_F(EvaluateConvertTest, NonIntegralTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to" << 3.6 << "onError" << 0));

    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document(), &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "In $convert, numeric 'to' argument is not an integer");
                             });
}

TEST_F(EvaluateConvertTest, NonStringNonNumericalTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to" << BSON("dinosaur" << "Tyrannosaurus rex")
                                                << "onError" << 0));

    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(Document(), &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "$convert's 'to' argument must be a string or number");
                             });
}

TEST_F(EvaluateConvertTest, InvalidNumericTargetTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to" << 100 << "onError" << 0));

    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(Document(), &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "In $convert, numeric value for 'to' does not correspond to a BSON type");
        });
}

TEST_F(EvaluateConvertTest, NegativeNumericTargetTypeFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to" << -2 << "onError" << 0));

    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(Document(), &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "In $convert, numeric value for 'to' does not correspond to a BSON type");
        });
}

void assertUnsupportedConversionBehavior(
    ExpressionContext* expCtx, std::vector<std::pair<Value, Value>> unsupportedConversions) {
    // Attempt all of the unsupported conversions listed above.
    for (const auto& conversion : unsupportedConversions) {
        auto inputValue = conversion.first;
        auto toValue = conversion.second;

        auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                    << "to" << toValue));

        Document input{{"path1", inputValue}};

        auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

        ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(input, &expCtx->variables),
                                 AssertionException,
                                 [](const AssertionException& exception) {
                                     ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                     ASSERT_STRING_CONTAINS(exception.reason(),
                                                            "Unsupported conversion");
                                 });
    }

    // Attempt them again, this time with an "onError" value.
    for (const auto& conversion : unsupportedConversions) {
        auto inputValue = conversion.first;
        auto toValue = conversion.second;

        auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                    << "to" << toValue << "onError"
                                                    << "X"));

        Document input{{"path1", inputValue}};

        auto convertExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);

        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(input, &expCtx->variables), "X"_sd, BSONType::string);
    }
}

TEST_F(EvaluateConvertTest, UnsupportedConversionShouldThrowUnlessOnErrorProvided) {
    std::vector<std::pair<Value, Value>> unsupportedConversions{
        // Except for the ones listed below, $convert supports all conversions between the supported
        // types: double, string, int, long, decimal, objectId, bool, int, and date.
        {Value(OID()), Value("double"_sd)},
        {Value(OID()), Value("int"_sd)},
        {Value(OID()), Value("long"_sd)},
        {Value(OID()), Value("decimal"_sd)},
        {Value(Date_t{}), Value("objectId"_sd)},
        {Value(Date_t{}), Value("int"_sd)},
        {Value(int{1}), Value("date"_sd)},
        {Value(true), Value("date"_sd)},

        // All conversions that involve any other type will fail, unless the target type is bool,
        // in which case the conversion results in a true value. Below is one conversion for each
        // of the unsupported types.
        {Value(1.0), Value("minKey"_sd)},
        {Value(1.0), Value("missing"_sd)},
        {Value(1.0), Value("object"_sd)},
        {Value(1.0), Value("array"_sd)},
        {Value(1.0), Value("undefined"_sd)},
        {Value(1.0), Value("null"_sd)},
        {Value(1.0), Value("regex"_sd)},
        {Value(1.0), Value("dbPointer"_sd)},
        {Value(1.0), Value("javascript"_sd)},
        {Value(1.0), Value("symbol"_sd)},
        {Value(1.0), Value("javascriptWithScope"_sd)},
        {Value(1.0), Value("timestamp"_sd)},
        {Value(1.0), Value("maxKey"_sd)},
    };

    assertUnsupportedConversionBehavior(getExpCtx().get(), std::move(unsupportedConversions));
}

TEST_F(EvaluateConvertTest, FeatureFlagGatedConversionShouldThrowUnlessOnErrorProvided) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagMqlJsEngineGap", false);

    Value str{"string"_sd};
    std::vector<std::pair<Value, Value>> unsupportedConversions{
        // Leaf types
        {Value(MINKEY), str},
        {Value(MAXKEY), str},
        {Value(BSONRegEx("^ABC"_sd, "i"_sd)), str},
        {Value(Timestamp(Seconds{1}, 2)), str},
        {Value(BSONDBRef("coll"_sd, OID::createFromString("0102030405060708090A0B0C"_sd))), str},
        {Value(BSONCodeWScope{"function() {}"_sd, BSONObj()}), str},
        {Value(BSONCode("function() {}"_sd)), str},
        {Value(BSONSymbol("foo"_sd)), str},
        // Nested types
        {Value(Document{{"foo", BSONNULL}}), str},
        {Value(std::vector<Value>{Value(Document()), Value()}), str},
    };

    assertUnsupportedConversionBehavior(getExpCtx().get(), std::move(unsupportedConversions));
}

TEST_F(EvaluateConvertTest, ConvertNullishInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document nullInput{{"path1", BSONNULL}};
    Document undefinedInput{{"path1", BSONUndefined}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput, &expCtx->variables), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput, &expCtx->variables), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput, &expCtx->variables), Value(BSONNULL));
}

TEST_F(EvaluateConvertTest, ConvertNullishInputWithOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "onNull"
                                                << "B)"
                                                << "onError"
                                                << "Should not be used here"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document nullInput{{"path1", BSONNULL}};
    Document undefinedInput{{"path1", BSONUndefined}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput, &expCtx->variables), Value("B)"_sd));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput, &expCtx->variables), Value("B)"_sd));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput, &expCtx->variables), Value("B)"_sd));
}

TEST_F(EvaluateConvertTest, NullishToReturnsNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "inputString"
                                                << "to"
                                                << "$path1"
                                                << "onNull"
                                                << "Should not be used here"
                                                << "onError"
                                                << "Also should not be used"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document nullInput{{"path1", BSONNULL}};
    Document undefinedInput{{"path1", BSONUndefined}};
    Document missingInput{{"path1", Value()}};

    ASSERT_VALUE_EQ(convertExp->evaluate(nullInput, &expCtx->variables), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(undefinedInput, &expCtx->variables), Value(BSONNULL));
    ASSERT_VALUE_EQ(convertExp->evaluate(missingInput, &expCtx->variables), Value(BSONNULL));
}

TEST_F(EvaluateConvertTest, NullInputOverridesNullTo) {
    auto expCtx = getExpCtx();

    auto spec =
        BSON("$convert" << BSON("input" << Value(BSONNULL) << "to" << Value(BSONNULL) << "onNull"
                                        << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(Document{}, &expCtx->variables), "X"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, DoubleIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "double"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document doubleInput{{"path1", 2.4}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInput, &expCtx->variables), 2.4, BSONType::numberDouble);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    auto result = convertExp->evaluate(doubleNaN, &expCtx->variables);
    ASSERT(std::isnan(result.getDouble()));

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    result = convertExp->evaluate(doubleInfinity, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_GT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    result = convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_LT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));
}

TEST_F(EvaluateConvertTest, BoolIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document trueBoolInput{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(trueBoolInput, &expCtx->variables), true, BSONType::boolean);

    Document falseBoolInput{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(falseBoolInput, &expCtx->variables), false, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, StringIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document stringInput{{"path1", "More cowbell"_sd}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(stringInput, &expCtx->variables), "More cowbell"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, ObjectIdIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "objectId"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("0123456789abcdef01234567")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(oidInput, &expCtx->variables),
                                   OID("0123456789abcdef01234567"),
                                   BSONType::oid);
}

TEST_F(EvaluateConvertTest, DateIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "date"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t{}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(dateInput, &expCtx->variables), Date_t{}, BSONType::date);
}

TEST_F(EvaluateConvertTest, IntIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document intInput{{"path1", int{123}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(intInput, &expCtx->variables), int{123}, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, LongIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document longInput{{"path1", 123LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(longInput, &expCtx->variables), 123LL, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, DecimalIdentityConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document decimalInput{{"path1", Decimal128("2.4")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInput, &expCtx->variables),
                                   Decimal128("2.4"),
                                   BSONType::numberDecimal);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalNaN, &expCtx->variables),
                                   Decimal128::kPositiveNaN,
                                   BSONType::numberDecimal);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(decimalInfinity, &expCtx->variables),
                                   Decimal128::kPositiveInfinity,
                                   BSONType::numberDecimal);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
        Decimal128::kNegativeInfinity,
        BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertToBinDataWithDefaultSubtypeSucceeds) {
    auto expCtx = getExpCtx();

    auto assertSucceedsWithDefaultSubtype = [&](const BSONObj spec) {
        auto convertExp =
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        std::string data = "(^_^)";
        BSONBinData input{data.data(), static_cast<int>(data.size()), BinDataGeneral};
        BSONBinData expected{data.data(), static_cast<int>(data.size()), BinDataGeneral};

        Document binDataInput{{"path1", input}};
        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(binDataInput, &expCtx->variables), expected, BSONType::binData);
    };

    // Test with a type string as the 'to' value.
    assertSucceedsWithDefaultSubtype(BSON("$convert" << BSON("input" << "$path1"
                                                                     << "to"
                                                                     << "binData")));

    // Test with an object without 'subtype' field as the 'to' value.
    assertSucceedsWithDefaultSubtype(
        BSON("$convert" << BSON("input" << "$path1"
                                        << "to" << BSON("type" << "binData"))));
}

TEST_F(EvaluateConvertTest, BinDataToBinDataConversionSameSubtype) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON(
                         "input" << "$path1"
                                 << "to"
                                 << BSON("type" << "binData"
                                                << "subtype" << static_cast<int>(BinDataGeneral))));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::string data = "(^_^)";
    BSONBinData input{data.data(), static_cast<int>(data.size()), BinDataGeneral};
    BSONBinData expected{data.data(), static_cast<int>(data.size()), BinDataGeneral};

    Document binDataInput{{"path1", input}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(binDataInput, &expCtx->variables), expected, BSONType::binData);
}

TEST_F(EvaluateConvertTest, BinDataToBinDataConversionDifferentSubtypesFails) {
    auto expCtx = getExpCtx();

    auto spec =
        BSON("$convert" << BSON("input" << "$path1"
                                        << "to"
                                        << BSON("type" << "binData"
                                                       << "subtype" << static_cast<int>(newUUID))));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::string data = "(^_^)";
    BSONBinData input{data.data(), static_cast<int>(data.size()), BinDataGeneral};
    BSONBinData expected{data.data(), static_cast<int>(data.size()), newUUID};

    Document binDataInput{{"path1", input}};
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(binDataInput, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "Conversions between different BinData subtypes are not supported");
        });
}

TEST_F(EvaluateConvertTest, Base64StringToNonUUIDBinDataConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON(
        "$convert" << BSON("input" << "$path1"
                                   << "to"
                                   << BSON("type" << "binData"
                                                  << "subtype" << static_cast<int>(BinDataGeneral))
                                   << "format" << toStringData(BinDataFormat::kBase64)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    const std::string data{"(^_^)"};
    const auto input = base64::encode(data);
    const BSONBinData expected{data.data(), static_cast<int>(data.size()), BinDataGeneral};

    Document stringInput{{"path1", input}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(stringInput, &expCtx->variables), expected, BSONType::binData);
}

TEST_F(EvaluateConvertTest, UUIDStringToNonUUIDBinDataConversionFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON(
        "$convert" << BSON("input" << "$path1"
                                   << "to"
                                   << BSON("type" << "binData"
                                                  << "subtype" << static_cast<int>(BinDataGeneral))
                                   << "format" << toStringData(BinDataFormat::kUuid)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    const std::string uuidString{"867dee52-c331-484e-92d1-c56479b8e67e"};

    Document stringInput{{"path1", uuidString}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(stringInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Only the 'uuid' format is allowed with the UUID subtype");
                             });
}

TEST_F(EvaluateConvertTest, InvalidStringToUUIDBinDataConversionFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$toUUID" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    // The input string is not a valid UUID string.
    const std::string input{"867dee52---484e-92d1"};

    Document stringInput{{"path1", input}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(stringInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Invalid UUID string");
                             });
}

TEST_F(EvaluateConvertTest, BinDataToStringConversionWithoutFormatFails) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    const std::string data{"(^_^)"};
    const BSONBinData input{data.data(), static_cast<int>(data.size()), BinDataGeneral};

    Document stringInput{{"path1", input}};
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate(stringInput, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), 4341115);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "Format must be speficied when converting from 'binData' to 'string'");
        });
}

TEST_F(EvaluateConvertTest, StringToUUIDBinDataConversion) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$toUUID" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    const auto uuid = UUID::gen();

    Document stringInput{{"path1", uuid.toString()}};
    ASSERT_EQ(convertExp->evaluate(stringInput, &expCtx->variables).getUuid(), uuid);
}

TEST_F(EvaluateConvertTest, UUIDStringRoundTripConversion) {
    const auto evalWithValueOnPath = [&](const BSONObj spec, const Value inputValue) {
        auto expCtx = getExpCtx();

        auto convertExp =
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document inputDoc{{"path1", inputValue}};
        return convertExp->evaluate(inputDoc, &expCtx->variables);
    };

    const Value stringValue{"867dee52-c331-484e-92d1-c56479b8e67e"_sd};

    const auto binDataValue = evalWithValueOnPath(BSON("$toUUID" << "$path1"), stringValue);

    const auto roundTripStringValue =
        evalWithValueOnPath(BSON("$toString" << "$path1"), binDataValue);
    ASSERT_VALUE_EQ(stringValue, roundTripStringValue);

    const auto roundTripBinDataValue =
        evalWithValueOnPath(BSON("$toUUID" << "$path1"), roundTripStringValue);
    ASSERT_VALUE_EQ(binDataValue, roundTripBinDataValue);
}

TEST_F(EvaluateConvertTest, ConvertDateToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    // All date inputs evaluate as true.
    Document dateInput{{"path1", Date_t{}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(dateInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertIntToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document trueIntInput{{"path1", int{1}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(trueIntInput, &expCtx->variables), true, BSONType::boolean);

    Document falseIntInput{{"path1", int{0}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(falseIntInput, &expCtx->variables), false, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertLongToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document trueLongInput{{"path1", -1LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(trueLongInput, &expCtx->variables), true, BSONType::boolean);

    Document falseLongInput{{"path1", 0LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(falseLongInput, &expCtx->variables), false, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertDoubleToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document trueDoubleInput{{"path1", 2.4}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(trueDoubleInput, &expCtx->variables), true, BSONType::boolean);

    Document falseDoubleInput{{"path1", -0.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(falseDoubleInput, &expCtx->variables), false, BSONType::boolean);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNaN, &expCtx->variables), true, BSONType::boolean);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInfinity, &expCtx->variables), true, BSONType::boolean);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertDecimalToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document trueDecimalInput{{"path1", Decimal128(5)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(trueDecimalInput, &expCtx->variables), true, BSONType::boolean);

    Document falseDecimalInput{{"path1", Decimal128(0)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(falseDecimalInput, &expCtx->variables), false, BSONType::boolean);

    Document preciseZero{{"path1", Decimal128("0.00")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(preciseZero, &expCtx->variables), false, BSONType::boolean);

    Document negativeZero{{"path1", Decimal128("-0.00")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeZero, &expCtx->variables), false, BSONType::boolean);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNaN, &expCtx->variables), true, BSONType::boolean);

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeNaN, &expCtx->variables), true, BSONType::boolean);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalInfinity, &expCtx->variables), true, BSONType::boolean);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertStringToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document stringInput{{"path1", "str"_sd}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(stringInput, &expCtx->variables), true, BSONType::boolean);

    Document emptyStringInput{{"path1", ""_sd}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(emptyStringInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertObjectIdToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("59E8A8D8FEDCBA9876543210")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(oidInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertMinKeyToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document minKeyInput{{"path1", MINKEY}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minKeyInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertObjectToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document objectInput{{"path1", Document{{"foo", 1}}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(objectInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertArrayToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document arrayInput{{"path1", BSON_ARRAY(1)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(arrayInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    char data[] = "(^_^)";
    Document binInput{{"path1", BSONBinData(data, sizeof(data), BinDataType::BinDataGeneral)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(binInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertRegexToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document regexInput{{"path1", BSONRegEx("ab*a"_sd)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(regexInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertDBRefToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document refInput{{"path1", BSONDBRef("db.coll"_sd, OID("aaaaaaaaaaaaaaaaaaaaaaaa"))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(refInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertCodeToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document codeInput{{"path1", BSONCode("print('Hello world!');"_sd)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(codeInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertSymbolToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document symbolInput{{"path1", BSONSymbol("print"_sd)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(symbolInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertCodeWScopeToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document codeWScopeInput{
        {"path1", BSONCodeWScope("print('Hello again, world!')"_sd, BSONObj())}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(codeWScopeInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertTimestampToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document timestampInput{{"path1", Timestamp()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(timestampInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertMaxKeyToBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "bool"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document maxKeyInput{{"path1", MAXKEY}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxKeyInput, &expCtx->variables), true, BSONType::boolean);
}

TEST_F(EvaluateConvertTest, ConvertAnyLeafValueToString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::vector<std::pair<Value, StringData>> cases{
        {Value(MINKEY), "MinKey"_sd},
        {Value(MAXKEY), "MaxKey"_sd},
        {Value(BSONRegEx("^ABC"_sd, "i"_sd)), "/^ABC/i"_sd},
        {Value(Timestamp(Seconds{1}, 2)), "Timestamp(1, 2)"_sd},
        {Value(BSONDBRef("coll"_sd, OID::createFromString("0102030405060708090A0B0C"_sd))),
         "DBRef(\"coll\", 0102030405060708090a0b0c)"_sd},
        {Value(BSONCodeWScope{"function() {}"_sd, BSONObj()}),
         "CodeWScope(\"function() {}\", {})"_sd},
        {Value(BSONCode("function() {}"_sd)), "function() {}"_sd},
        {Value(BSONSymbol("foo"_sd)), "foo"_sd},
    };

    for (auto&& [in, out] : cases) {
        Document inputDoc{{"path1", std::move(in)}};
        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(inputDoc, &expCtx->variables), out, BSONType::string);
    }

    Document inputDoc{{"path1", Value(BSONUndefined)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(inputDoc, &expCtx->variables), BSONNULL, BSONType::null);
}

TEST_F(EvaluateConvertTest, ConvertAnyNestedValueToString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document obj{
        {"arr",
         std::vector<Value>{
             Value(BSONNULL),
             Value(BSONUndefined),
             Value(MINKEY),
             Value(MAXKEY),
             Value(BSONRegEx("^ABC"_sd, "i"_sd)),
             Value(Timestamp(Seconds{1}, 2)),
             Value(BSONDBRef("coll"_sd, OID::createFromString("0102030405060708090A0B0C"_sd))),
             Value(BSONCodeWScope{"function() {}"_sd, BSONObj()}),
             Value(BSONCode("function() {}"_sd)),
             Value(BSONSymbol("foo"_sd)),
             Value(Document{{"obj", Value(1)}}),
             Value(std::vector<Value>{
                 Value(1),
                 Value(std::vector<Value>{Value("foo"_sd)}),
             }),
         }}};

    // Test with top-level object.
    {
        const std::string expected =
            "{\"arr\":[null,null,\"MinKey\",\"MaxKey\",\"/^ABC/i\",\"Timestamp(1, "
            "2)\",\"DBRef(\\\"coll\\\", 0102030405060708090a0b0c)\",\"CodeWScope(\\\"function() "
            "{}\\\", {})\",\"function() {}\",\"foo\",{\"obj\":1},[1,[\"foo\"]]]}";
        Document inputDoc{{"path1", obj}};
        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(inputDoc, &expCtx->variables), expected, BSONType::string);
    }

    // Test with top-level array.
    {
        std::vector<Value> arr{Value(std::move(obj))};
        const std::string expected =
            "[{\"arr\":[null,null,\"MinKey\",\"MaxKey\",\"/^ABC/i\",\"Timestamp(1, "
            "2)\",\"DBRef(\\\"coll\\\", 0102030405060708090a0b0c)\",\"CodeWScope(\\\"function() "
            "{}\\\", {})\",\"function() {}\",\"foo\",{\"obj\":1},[1,[\"foo\"]]]}]";
        Document inputDoc{{"path1", std::move(arr)}};
        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(inputDoc, &expCtx->variables), expected, BSONType::string);
    }
}

TEST_F(EvaluateConvertTest, ConvertStringToObject) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$convert" << BSON("input" << "$path1" << "to" << "object"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::vector<std::pair<StringData, BSONObj>> cases{
        {"{}"_sd, BSONObj()},
        {"{\"\": \"emptyKey\"}"_sd, BSON("" << "emptyKey")},
        {"{\"foo\": \"bar\"}"_sd, BSON("foo" << "bar")},
        {"{\"foo\": null}"_sd, BSON("foo" << BSONNULL)},
        {"{\"foo\": false}"_sd, BSON("foo" << false)},
        {"{\"foo\": true}"_sd, BSON("foo" << true)},
        {"{\"foo\": 123}"_sd, BSON("foo" << 123)},
        // Embedded nulls are allowed in values
        {"{\"name\": \"fo\\u0000o\"}"_sd, BSON("name" << "fo\u0000o"_sd)},
        // Duplicate field names are allowed, we keep the last value.
        {"{\"a\": 1, \"b\": 2, \"a\": 3}"_sd, BSON("a" << 3 << "b" << 2)},
        // Nested objects
        {"{\"__proto__\": { \"foo\": null }}"_sd, BSON("__proto__" << BSON("foo" << BSONNULL))},
        {"{\"objectId\": \"507f1f77bcf86cd799439011\", \"uuid\": "
         "\"3b241101-e2bb-4255-8caf-4136c566a962\", \"date\": \"2018-03-27T16:58:51.538Z\", "
         "\"regex\": \"/^ABC/i\", \"js\": \"function (s) {return s + \\\"foo\\\";}\", "
         "\"timestamp\": \"Timestamp(1565545664, 1)\", \"arr\": [1,2,3], \"obj\": {\"in\": 1}}",
         BSON("objectId" << "507f1f77bcf86cd799439011" << "uuid"
                         << "3b241101-e2bb-4255-8caf-4136c566a962" << "date"
                         << "2018-03-27T16:58:51.538Z" << "regex" << "/^ABC/i" << "js"
                         << "function (s) {return s + \"foo\";}" << "timestamp"
                         << "Timestamp(1565545664, 1)" << "arr" << BSON_ARRAY(1 << 2 << 3) << "obj"
                         << BSON("in" << 1))},
    };

    for (auto&& [input, expected] : cases) {
        Document inputDoc{{"path1", Value(input)}};
        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(inputDoc, &expCtx->variables), expected, BSONType::object);
    }
}

TEST_F(EvaluateConvertTest, ConvertStringToArray) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$convert" << BSON("input" << "$path1" << "to" << "array"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::vector<std::pair<StringData, BSONArray>> cases{
        {"[]"_sd, BSONArray()},
        {"[\"bar\"]"_sd, BSON_ARRAY("bar")},
        {"[null]"_sd, BSON_ARRAY(BSONNULL)},
        {"[false]"_sd, BSON_ARRAY(false)},
        {"[true]"_sd, BSON_ARRAY(true)},
        {"[123]"_sd, BSON_ARRAY(123)},
        // Embedded nulls are allowed in values
        {"[\"fo\\u0000o\"]"_sd, BSON_ARRAY("fo\u0000o"_sd)},
        // Nested arrays
        {"[[1,2],{\"__proto__\": { \"foo\": null }}]"_sd,
         BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON("__proto__" << BSON("foo" << BSONNULL)))},
        {"[1, {\"objectId\": \"507f1f77bcf86cd799439011\", \"uuid\": "
         "\"3b241101-e2bb-4255-8caf-4136c566a962\", \"date\": \"2018-03-27T16:58:51.538Z\", "
         "\"regex\": \"/^ABC/i\", \"js\": \"function (s) {return s + \\\"foo\\\";}\", "
         "\"timestamp\": \"Timestamp(1565545664, 1)\", \"arr\": [1,2,3], \"obj\": {\"in\": 1}}, "
         "[null]]",
         BSON_ARRAY(1 << BSON("objectId"
                              << "507f1f77bcf86cd799439011" << "uuid"
                              << "3b241101-e2bb-4255-8caf-4136c566a962" << "date"
                              << "2018-03-27T16:58:51.538Z" << "regex" << "/^ABC/i" << "js"
                              << "function (s) {return s + \"foo\";}" << "timestamp"
                              << "Timestamp(1565545664, 1)" << "arr" << BSON_ARRAY(1 << 2 << 3)
                              << "obj" << BSON("in" << 1))
                      << BSON_ARRAY(BSONNULL))},
    };

    for (auto&& [input, expected] : cases) {
        Document inputDoc{{"path1", Value(input)}};
        ASSERT_VALUE_CONTENTS_AND_TYPE(
            convertExp->evaluate(inputDoc, &expCtx->variables), expected, BSONType::array);
    }
}

TEST_F(EvaluateConvertTest, ConvertStringToObjectOrArrayNumberHandling) {
    auto expCtx = getExpCtx();

    auto toObjectSpec = BSON("$convert" << BSON("input" << "$path1" << "to" << "object"));
    auto toObjectExp =
        Expression::parseExpression(expCtx.get(), toObjectSpec, expCtx->variablesParseState);

    auto toArraySpec = BSON("$convert" << BSON("input" << "$path1" << "to" << "array"));
    auto toArrayExp =
        Expression::parseExpression(expCtx.get(), toArraySpec, expCtx->variablesParseState);

    auto assertParsedNumberEquals = [&](StringData numStr,
                                        Value expectedValue,
                                        BSONType expectedType) {
        // Test inside object.
        {
            std::string input = str::stream() << "{\"f\":" << numStr << "}";
            Document inputDoc{{"path1", Value(input)}};
            auto result = toObjectExp->evaluate(inputDoc, &expCtx->variables);
            ASSERT_EQ(result.getType(), BSONType::object);
            ASSERT_VALUE_CONTENTS_AND_TYPE(
                result.getDocument().getField("f"_sd), expectedValue, expectedType);
        }
        // Test inside array.
        {
            std::string input = str::stream() << "[" << numStr << "]";
            Document inputDoc{{"path1", Value(input)}};
            auto result = toArrayExp->evaluate(inputDoc, &expCtx->variables);
            ASSERT_EQ(result.getType(), BSONType::array);
            ASSERT_VALUE_CONTENTS_AND_TYPE(result.getArray().front(), expectedValue, expectedType);
        }
    };

    assertParsedNumberEquals("\"NaN\""_sd, Value("NaN"_sd), BSONType::string);
    assertParsedNumberEquals("123"_sd, Value(123), BSONType::numberInt);
    assertParsedNumberEquals("-123"_sd, Value(-123), BSONType::numberInt);
    assertParsedNumberEquals("4294967296"_sd, Value(4294967296LL), BSONType::numberLong);
    assertParsedNumberEquals("-4294967296"_sd, Value(-4294967296LL), BSONType::numberLong);
    assertParsedNumberEquals("1.123123"_sd, Value(1.123123), BSONType::numberDouble);
    assertParsedNumberEquals("-1.123123"_sd, Value(-1.123123), BSONType::numberDouble);
    assertParsedNumberEquals("1.2e+3"_sd, Value(1200.0), BSONType::numberDouble);
    assertParsedNumberEquals("-1.2e+3"_sd, Value(-1200.0), BSONType::numberDouble);
    // This would fit in a 64-bit unsigned integer but BSON doesn't have that.
    assertParsedNumberEquals(
        "18446744073709551615"_sd, Value(18446744073709551615.0), BSONType::numberDouble);
    assertParsedNumberEquals(
        "-18446744073709551615"_sd, Value(-18446744073709551615.0), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertStringToObjectOrArrayInvalidConversions) {
    auto expCtx = getExpCtx();

    auto toObjectExp = Expression::parseExpression(
        expCtx.get(),
        BSON("$convert" << BSON("input" << "$path1" << "to" << "object")),
        expCtx->variablesParseState);

    auto toObjectWithOnErrorExp = Expression::parseExpression(
        expCtx.get(),
        BSON("$convert" << BSON("input" << "$path1" << "to" << "object" << "onError" << "error!")),
        expCtx->variablesParseState);

    auto toArrayExp = Expression::parseExpression(
        expCtx.get(),
        BSON("$convert" << BSON("input" << "$path1" << "to" << "array")),
        expCtx->variablesParseState);

    auto toArrayWithOnErrorExp = Expression::parseExpression(
        expCtx.get(),
        BSON("$convert" << BSON("input" << "$path1" << "to" << "array" << "onError" << "error!")),
        expCtx->variablesParseState);

    auto assertThrowsInvalidJson = [&](StringData input) {
        Document inputDoc{{"path1", Value(input)}};

        // Test without 'onError'.
        for (const auto& exp : {toObjectExp, toArrayExp}) {
            ASSERT_THROWS_WITH_CHECK(exp->evaluate(inputDoc, &expCtx->variables),
                                     AssertionException,
                                     [](const AssertionException& exception) {
                                         ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                         ASSERT_STRING_CONTAINS(
                                             exception.reason(),
                                             "Input doesn't represent valid JSON"_sd);
                                     });
        }

        // Test with 'onError'.
        for (const auto& exp : {toObjectWithOnErrorExp, toArrayWithOnErrorExp}) {
            ASSERT_VALUE_CONTENTS_AND_TYPE(
                exp->evaluate(inputDoc, &expCtx->variables), "error!"_sd, BSONType::string);
        }
    };

    // Some examples of invalid JSON. The main purpose of these is to ensure that we either handle a
    // parsing error correctly or throw the right error when parsing event handlers are called in an
    // unexpected order. The latter can happen before the parsing library parses far enough to
    // encounter a syntax error. E.g. when an opening brace is missing, the parser won't detect
    // invalid syntax until it reaches the unmatched closing brace.
    //
    // For more exhaustive testing we rely on libfuzz. Some specific security concerns like embedded
    // nulls are also tested below.
    assertThrowsInvalidJson("}\"closingBraceFirst1\": 123{"_sd);
    assertThrowsInvalidJson("}\"closingBraceFirst2\": 123}"_sd);
    assertThrowsInvalidJson("]\"closingBracketFirst1\": 123["_sd);
    assertThrowsInvalidJson("]\"closingBracketFirst2\": 123]"_sd);
    assertThrowsInvalidJson("{\"missingClosingBrace\": 123"_sd);
    assertThrowsInvalidJson("\"missingOpeningBrace\": 123}"_sd);
    assertThrowsInvalidJson("[\"missingClosingBracket\": 123"_sd);
    assertThrowsInvalidJson("\"missingOpeningBracket\": 123]"_sd);
    assertThrowsInvalidJson("{\"missingClosingQuote: 123}"_sd);
    assertThrowsInvalidJson("[\"bracketMismatch1\": 123}"_sd);
    assertThrowsInvalidJson("{\"bracketMismatch2\": 123]"_sd);
    assertThrowsInvalidJson("{\"bracketMismatch3\"]"_sd);
    assertThrowsInvalidJson("[\"bracketMismatch4\"}"_sd);
    assertThrowsInvalidJson("{\"extraClosingBrace\": 123}}"_sd);
    assertThrowsInvalidJson("{{\"extraOpeningBrace\": 123}"_sd);
    assertThrowsInvalidJson("[\"extraClosingBracket\"]]"_sd);
    assertThrowsInvalidJson("[[\"extraOpeningBracket\"]"_sd);
    assertThrowsInvalidJson("null: null}"_sd);
    assertThrowsInvalidJson("{null: null}"_sd);
    assertThrowsInvalidJson("{\"semicolon\"; null}"_sd);
    assertThrowsInvalidJson("{\"oid\": ObjectId(\"6592008029c8c3e4dc76256c\")}"_sd);
    assertThrowsInvalidJson("{\"multilinejson\": 1}\n{\"multilinejson\": 2}"_sd);
    assertThrowsInvalidJson("[{\"multilinejson\": 1}\n{\"multilinejson\": 2}]"_sd);
    assertThrowsInvalidJson("{\"trailingComma\": 123,}"_sd);
    assertThrowsInvalidJson("[1, 2, 3, ]"_sd);
    assertThrowsInvalidJson("[1, 2, , 3]"_sd);
    assertThrowsInvalidJson("{\"missingColon\" 123}"_sd);
    assertThrowsInvalidJson("{unquotedKey: 123}"_sd);
    assertThrowsInvalidJson("{\"invalidLiteral\": nope}"_sd);
    assertThrowsInvalidJson("{'singleQuotes': 'invalid'}"_sd);
    assertThrowsInvalidJson("{\"extraToken\": 1} 123"_sd);
    // Control character in string without escaping
    assertThrowsInvalidJson("{\"foo\": \"bar\tbaz\"}"_sd);
    // Backslash at end of string
    assertThrowsInvalidJson("{\"foo\": \"bar\\\"}"_sd);
    // Invalid UTF-8
    assertThrowsInvalidJson("{\"\xC3\x28\": 123}"_sd);
    // Bad unicode
    assertThrowsInvalidJson("{\"badUnicode\": \"\\uZZZZ\"}"_sd);
    assertThrowsInvalidJson("{\"tooLargeCodepoint\": \"\\u{110000}\"}"_sd);
    assertThrowsInvalidJson("{\"orphanSurrogate\": \"\\uD800\"}"_sd);
    assertThrowsInvalidJson("{\"badSurrogates\": \"\\uDC00\\uD800\"}"_sd);
    // NaN or Infinity (valid in JS, invalid in JSON)
    assertThrowsInvalidJson("{\"value\": NaN}"_sd);
    assertThrowsInvalidJson("{\"value\": Infinity}"_sd);
    // Leading + before a number
    assertThrowsInvalidJson("{\"plusNumber\": +42}"_sd);
    // Escaped null byte in field name
    assertThrowsInvalidJson("{\"fo\\u0000o\": 123}"_sd);
    // Unescaped null byte in field name
    assertThrowsInvalidJson("{\"fo\0o\": 123}"_sd);
    // Unescaped null byte in value
    assertThrowsInvalidJson("{\"foo\": \"fo\0o\"}"_sd);

    // Type mismatch
    ASSERT_THROWS_WITH_CHECK(
        toArrayExp->evaluate(Document{{"path1", Value("{\"foo\": 1}"_sd)}}, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Input doesn't match expected type 'array'"_sd);
        });
    ASSERT_THROWS_WITH_CHECK(
        toObjectExp->evaluate(Document{{"path1", Value("[{\"foo\": 1}]"_sd)}}, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Input doesn't match expected type 'object'"_sd);
        });
}

TEST_F(EvaluateConvertTest, ConvertNumericToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "double"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document intInput{{"path1", int{1}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(intInput, &expCtx->variables), 1.0, BSONType::numberDouble);

    Document longInput{{"path1", 0xf00000000LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(longInput, &expCtx->variables), 64424509440.0, BSONType::numberDouble);

    Document decimalInput{{"path1", Decimal128("5.5")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalInput, &expCtx->variables), 5.5, BSONType::numberDouble);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolFalse, &expCtx->variables), 0.0, BSONType::numberDouble);

    Document boolTrue{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolTrue, &expCtx->variables), 1.0, BSONType::numberDouble);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    auto result = convertExp->evaluate(decimalNaN, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT(std::isnan(result.getDouble()));

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    result = convertExp->evaluate(decimalNegativeNaN, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT(std::isnan(result.getDouble()));

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    result = convertExp->evaluate(decimalInfinity, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_GT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    result = convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_LT(result.getDouble(), 0.0);
    ASSERT(std::isinf(result.getDouble()));

    // Note that the least significant bits get lost, because the significand of a double is not
    // wide enough for the original long long value in its entirety.
    Document largeLongInput{{"path1", 0xf0000000000000fLL}};
    result = convertExp->evaluate(largeLongInput, &expCtx->variables);
    ASSERT_EQ(static_cast<long long>(result.getDouble()), 0xf00000000000000LL);

    // Again, some precision is lost in the conversion from Decimal128 to double.
    Document preciseDecimalInput{{"path1", Decimal128("1.125000000000000000005")}};
    result = convertExp->evaluate(preciseDecimalInput, &expCtx->variables);
    ASSERT_EQ(result.getDouble(), 1.125);
}

TEST_F(EvaluateConvertTest, ConvertDateToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "double"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(123)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(dateInput, &expCtx->variables), 123.0, BSONType::numberDouble);

    // Note that the least significant bits get lost, because the significand of a double is not
    // wide enough for the original 64-bit Date_t value in its entirety.
    Document largeDateInput{{"path1", Date_t::fromMillisSinceEpoch(0xf0000000000000fLL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(largeDateInput, &expCtx->variables),
                                   0xf00000000000000LL,
                                   BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDecimalToDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "double"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document overflowInput{{"path1", Decimal128("1e309")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document negativeOverflowInput{{"path1", Decimal128("-1e309")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDecimalToDoubleWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "double"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document overflowInput{{"path1", Decimal128("1e309")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(overflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document negativeOverflowInput{{"path1", Decimal128("-1e309")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertNumericToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document intInput{{"path1", int{1}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(intInput, &expCtx->variables), Decimal128(1), BSONType::numberDecimal);

    Document longInput{{"path1", 0xf00000000LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(longInput, &expCtx->variables),
                                   Decimal128(std::int64_t{0xf00000000LL}),
                                   BSONType::numberDecimal);

    Document doubleInput{{"path1", 0.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInput, &expCtx->variables),
                                   Decimal128("0.1"),
                                   BSONType::numberDecimal);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(boolFalse, &expCtx->variables),
                                   Decimal128(0),
                                   BSONType::numberDecimal);

    Document boolTrue{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolTrue, &expCtx->variables), Decimal128(1), BSONType::numberDecimal);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleNaN, &expCtx->variables),
                                   Decimal128::kPositiveNaN,
                                   BSONType::numberDecimal);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleInfinity, &expCtx->variables),
                                   Decimal128::kPositiveInfinity,
                                   BSONType::numberDecimal);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables),
                                   Decimal128::kNegativeInfinity,
                                   BSONType::numberDecimal);

    // Unlike the similar conversion in ConvertNumericToDouble, there is more than enough precision
    // to store the exact orignal value in a Decimal128.
    Document largeLongInput{{"path1", 0xf0000000000000fLL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(largeLongInput, &expCtx->variables),
                                   0xf0000000000000fLL,
                                   BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertDateToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "decimal"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(123)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(dateInput, &expCtx->variables),
                                   Decimal128(123),
                                   BSONType::numberDecimal);

    Document largeDateInput{{"path1", Date_t::fromMillisSinceEpoch(0xf0000000000000fLL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(largeDateInput, &expCtx->variables),
                                   0xf0000000000000fLL,
                                   BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertDoubleToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput, &expCtx->variables), 1, BSONType::numberInt);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", 2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput1, &expCtx->variables), 2, BSONType::numberInt);

    Document nonIntegerInput2{{"path1", 2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput2, &expCtx->variables), 2, BSONType::numberInt);

    Document nonIntegerInput3{{"path1", -2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3, &expCtx->variables), -2, BSONType::numberInt);

    Document nonIntegerInput4{{"path1", -2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4, &expCtx->variables), -2, BSONType::numberInt);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", static_cast<double>(maxInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput, &expCtx->variables), maxInt, BSONType::numberInt);

    int minInt = std::numeric_limits<int>::lowest();
    Document minInput{{"path1", static_cast<double>(minInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minInput, &expCtx->variables), minInt, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDoubleToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    double overflowInt =
        std::nextafter(static_cast<double>(maxInt), std::numeric_limits<double>::max());
    Document overflowInput{{"path1", overflowInt}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput, &expCtx->variables),
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
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDoubleToIntWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    double overflowInt =
        std::nextafter(static_cast<double>(maxInt), std::numeric_limits<double>::max());
    Document overflowInput{{"path1", overflowInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(overflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    int minInt = std::numeric_limits<int>::lowest();
    double negativeOverflowInt =
        std::nextafter(static_cast<double>(minInt), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", negativeOverflowInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nanInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables), "X"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertDoubleToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput, &expCtx->variables), 1, BSONType::numberLong);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", 2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput1, &expCtx->variables), 2, BSONType::numberLong);

    Document nonIntegerInput2{{"path1", 2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput2, &expCtx->variables), 2, BSONType::numberLong);

    Document nonIntegerInput3{{"path1", -2.1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3, &expCtx->variables), -2, BSONType::numberLong);

    Document nonIntegerInput4{{"path1", -2.9}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4, &expCtx->variables), -2, BSONType::numberLong);

    // maxVal is the highest double value that will not overflow long long.
    double maxVal = std::nextafter(BSONElement::kLongLongMaxPlusOneAsDouble, 0.0);
    Document maxInput{{"path1", maxVal}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(maxInput, &expCtx->variables),
                                   static_cast<long long>(maxVal),
                                   BSONType::numberLong);

    // minVal is the lowest double value that will not overflow long long.
    double minVal = static_cast<double>(std::numeric_limits<long long>::lowest());
    Document minInput{{"path1", minVal}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(minInput, &expCtx->variables),
                                   static_cast<long long>(minVal),
                                   BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDoubleToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    double overflowLong = BSONElement::kLongLongMaxPlusOneAsDouble;
    Document overflowInput{{"path1", overflowLong}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput, &expCtx->variables),
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
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDoubleToLongWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    double overflowLong = BSONElement::kLongLongMaxPlusOneAsDouble;
    Document overflowInput{{"path1", overflowLong}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(overflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    double minLong = static_cast<double>(std::numeric_limits<long long>::lowest());
    double negativeOverflowLong =
        std::nextafter(static_cast<double>(minLong), std::numeric_limits<double>::lowest());
    Document negativeOverflowInput{{"path1", negativeOverflowLong}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nanInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables), "X"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertDecimalToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Decimal128("1.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput, &expCtx->variables), 1, BSONType::numberInt);

    // Conversions to int should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Decimal128("2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput1, &expCtx->variables), 2, BSONType::numberInt);

    Document nonIntegerInput2{{"path1", Decimal128("2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput2, &expCtx->variables), 2, BSONType::numberInt);

    Document nonIntegerInput3{{"path1", Decimal128("-2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3, &expCtx->variables), -2, BSONType::numberInt);

    Document nonIntegerInput4{{"path1", Decimal128("-2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3, &expCtx->variables), -2, BSONType::numberInt);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", Decimal128(maxInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput, &expCtx->variables), maxInt, BSONType::numberInt);

    int minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", Decimal128(minInt)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minInput, &expCtx->variables), minInt, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDecimalToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", Decimal128(maxInt).add(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    int minInt = std::numeric_limits<int>::lowest();
    Document negativeOverflowInput{{"path1", Decimal128(minInt).subtract(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeNaNInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDecimalToIntWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    int maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", Decimal128(maxInt).add(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(overflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    int minInt = std::numeric_limits<int>::lowest();
    Document negativeOverflowInput{{"path1", Decimal128(minInt).subtract(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nanInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeNaNInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
        "X"_sd,
        BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertDecimalToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", Decimal128("1.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput, &expCtx->variables), 1, BSONType::numberLong);

    // Conversions to long should always truncate the fraction (i.e., round towards 0).
    Document nonIntegerInput1{{"path1", Decimal128("2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput1, &expCtx->variables), 2, BSONType::numberLong);

    Document nonIntegerInput2{{"path1", Decimal128("2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput2, &expCtx->variables), 2, BSONType::numberLong);

    Document nonIntegerInput3{{"path1", Decimal128("-2.1")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput3, &expCtx->variables), -2, BSONType::numberLong);

    Document nonIntegerInput4{{"path1", Decimal128("-2.9")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nonIntegerInput4, &expCtx->variables), -2, BSONType::numberLong);

    long long maxVal = std::numeric_limits<long long>::max();
    Document maxInput{{"path1", Decimal128(std::int64_t{maxVal})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput, &expCtx->variables), maxVal, BSONType::numberLong);

    long long minVal = std::numeric_limits<long long>::min();
    Document minInput{{"path1", Decimal128(std::int64_t{minVal})}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minInput, &expCtx->variables), minVal, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDecimalToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    long long maxVal = std::numeric_limits<long long>::max();
    Document overflowInput{{"path1", Decimal128(std::int64_t{maxVal}).add(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    long long minVal = std::numeric_limits<long long>::lowest();
    Document negativeOverflowInput{
        {"path1", Decimal128(std::int64_t{minVal}).subtract(Decimal128(1))}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(nanInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeNaNInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Attempt to convert NaN value to integer");
                             });

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsDecimalToLongWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    long long maxVal = std::numeric_limits<long long>::max();
    Document overflowInput{{"path1", Decimal128(std::int64_t{maxVal}).add(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(overflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    long long minVal = std::numeric_limits<long long>::lowest();
    Document negativeOverflowInput{
        {"path1", Decimal128(std::int64_t{minVal}).subtract(Decimal128(1))}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nanInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeNaNInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
        "X"_sd,
        BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertDateToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(123LL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(dateInput, &expCtx->variables), 123LL, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertIntToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput, &expCtx->variables), 1LL, BSONType::numberLong);

    int maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", maxInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput, &expCtx->variables), maxInt, BSONType::numberLong);

    int minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", minInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minInput, &expCtx->variables), minInt, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertLongToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document simpleInput{{"path1", 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(simpleInput, &expCtx->variables), 1, BSONType::numberInt);

    long long maxInt = std::numeric_limits<int>::max();
    Document maxInput{{"path1", maxInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(maxInput, &expCtx->variables), maxInt, BSONType::numberInt);

    long long minInt = std::numeric_limits<int>::min();
    Document minInput{{"path1", minInt}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(minInput, &expCtx->variables), minInt, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsLongToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    long long maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", maxInt + 1}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(overflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    long long minInt = std::numeric_limits<int>::min();
    Document negativeOverflowInput{{"path1", minInt - 1}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(negativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsLongToIntWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    long long maxInt = std::numeric_limits<int>::max();
    Document overflowInput{{"path1", maxInt + 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(overflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    long long minInt = std::numeric_limits<int>::min();
    Document negativeOverflowInput{{"path1", minInt - 1}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertBoolToInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolFalse, &expCtx->variables), 0, BSONType::numberInt);

    Document boolTrue{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolTrue, &expCtx->variables), 1, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertBoolToLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "long"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document boolFalse{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolFalse, &expCtx->variables), 0LL, BSONType::numberLong);

    Document boolTrue{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(boolTrue, &expCtx->variables), 1LL, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertNumberToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "date"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document longInput{{"path1", 0LL}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(longInput, &expCtx->variables).getDate()),
              "1970-01-01T00:00:00.000Z");

    Document doubleInput{{"path1", 431568000000.0}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(doubleInput, &expCtx->variables).getDate()),
              "1983-09-05T00:00:00.000Z");

    Document doubleInputWithFraction{{"path1", 431568000000.987}};
    ASSERT_EQ(dateToISOStringUTC(
                  convertExp->evaluate(doubleInputWithFraction, &expCtx->variables).getDate()),
              "1983-09-05T00:00:00.000Z");

    Document decimalInput{{"path1", Decimal128("872835240000")}};
    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(decimalInput, &expCtx->variables).getDate()),
              "1997-08-29T06:14:00.000Z");

    Document decimalInputWithFraction{{"path1", Decimal128("872835240000.987")}};
    ASSERT_EQ(dateToISOStringUTC(
                  convertExp->evaluate(decimalInputWithFraction, &expCtx->variables).getDate()),
              "1997-08-29T06:14:00.000Z");
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsNumberToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "date"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document doubleOverflowInput{{"path1", 1.0e100}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document doubleNegativeOverflowInput{{"path1", -1.0e100}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNaN, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert NaN value to integer type");
                             });

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });

    Document decimalOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document decimalNegativeOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeOverflowInput, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Conversion would overflow target type");
                             });

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNaN, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert NaN value to integer type");
                             });

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeNaN, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert NaN value to integer type");
                             });

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Attempt to convert infinity value to integer type");
                             });
}

TEST_F(EvaluateConvertTest, ConvertOutOfBoundsNumberToDateWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "date"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    // Int is explicitly disallowed for date conversions. Clients must use 64-bit long instead.
    Document intInput{{"path1", int{0}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(intInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleOverflowInput{{"path1", 1.0e100}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleNegativeOverflowInput{{"path1", -1.0e100}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeOverflowInput, &expCtx->variables),
        "X"_sd,
        BSONType::string);

    Document doubleNaN{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNaN, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleInfinity{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document doubleNegativeInfinity{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(doubleNegativeInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalOverflowInput, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalNegativeOverflowInput{{"path1", Decimal128("1.0e100")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeOverflowInput, &expCtx->variables),
        "X"_sd,
        BSONType::string);

    Document decimalNaN{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNaN, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalNegativeNaN{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeNaN, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalInfinity{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalInfinity, &expCtx->variables), "X"_sd, BSONType::string);

    Document decimalNegativeInfinity{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(decimalNegativeInfinity, &expCtx->variables),
        "X"_sd,
        BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertObjectIdToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "date"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document oidInput{{"path1", OID("59E8A8D8FEDCBA9876543210")}};

    ASSERT_EQ(dateToISOStringUTC(convertExp->evaluate(oidInput, &expCtx->variables).getDate()),
              "2017-10-19T13:30:00.000Z");
}

TEST_F(EvaluateConvertTest, ConvertTimestampToDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "date"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document timestampInput{{"path1", Timestamp(1508419800, 1)}};

    ASSERT_EQ(
        dateToISOStringUTC(convertExp->evaluate(timestampInput, &expCtx->variables).getDate()),
        "2017-10-19T13:30:00.000Z");
}

TEST_F(EvaluateConvertTest, ConvertStringToInt) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'int'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5, BSONType::numberInt);

    spec = fromjson("{$convert: {input: '" + std::to_string(kIntMax) + "', to: 'int'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), kIntMax, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertStringToIntOverflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '" + std::to_string(kIntMax + 1) + "', to: 'int'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
                             });

    spec = fromjson("{$convert: {input: '" + std::to_string(kIntMin - 1) + "', to: 'int'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToIntOverflowWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec = fromjson("{$convert: {input: '" + std::to_string(kIntMax + 1) +
                         "', to: 'int', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '" + std::to_string(kIntMin - 1) +
                    "', to: 'int', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertStringToLong) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'long'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5LL, BSONType::numberLong);

    spec = fromjson("{$convert: {input: '" + std::to_string(kLongMax) + "', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), kLongMax, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertStringToLongOverflow) {
    auto expCtx = getExpCtx();
    auto longMaxPlusOneAsString = std::to_string(BSONElement::kLongLongMaxPlusOneAsDouble);
    // Remove digits after the decimal to avoid parse failure.
    longMaxPlusOneAsString = longMaxPlusOneAsString.substr(0, longMaxPlusOneAsString.find('.'));

    auto spec = fromjson("{$convert: {input: '" + longMaxPlusOneAsString + "', to: 'long'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
                             });

    auto longMinMinusOneAsString = std::to_string(kLongNegativeOverflow);
    longMinMinusOneAsString = longMinMinusOneAsString.substr(0, longMinMinusOneAsString.find('.'));

    spec = fromjson("{$convert: {input: '" + longMinMinusOneAsString + "', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Overflow");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToLongFailsForFloats) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5.5', to: 'long'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Did not consume whole string");
                             });

    spec = fromjson("{$convert: {input: '5.0', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Did not consume whole string");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToLongWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;
    auto longMaxPlusOneAsString = std::to_string(BSONElement::kLongLongMaxPlusOneAsDouble);
    // Remove digits after the decimal to avoid parse failure.
    longMaxPlusOneAsString = longMaxPlusOneAsString.substr(0, longMaxPlusOneAsString.find('.'));

    auto spec = fromjson("{$convert: {input: '" + longMaxPlusOneAsString +
                         "', to: 'long', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    auto longMinMinusOneAsString = std::to_string(kLongNegativeOverflow);
    longMinMinusOneAsString = longMinMinusOneAsString.substr(0, longMinMinusOneAsString.find('.'));

    spec = fromjson("{$convert: {input: '" + longMinMinusOneAsString + "', to: 'long', onError: '" +
                    onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '5.5', to: 'long', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '5.0', to: 'long', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertStringToDouble) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5.0, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '5.5', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5.5, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '.5', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 0.5, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '+5', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5.0, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '+5.0e42', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5.0e42, BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertStringToDoubleWithPrecisionLoss) {
    auto expCtx = getExpCtx();

    // Note that the least significant bits get lost, because the significand of a double is not
    // wide enough for the given input string in its entirety.
    auto spec = fromjson("{$convert: {input: '10000000000000000001', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 1e19, BSONType::numberDouble);

    // Again, some precision is lost in the conversion to double.
    spec = fromjson("{$convert: {input: '1.125000000000000000005', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 1.125, BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertStringToDoubleFailsForInvalidFloats) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '.5.', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Did not consume whole string");
                             });

    spec = fromjson("{$convert: {input: '5.5f', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Did not consume whole string");
                             });
}

TEST_F(EvaluateConvertTest, ConvertInfinityStringsToDouble) {
    auto expCtx = getExpCtx();
    auto infValue = std::numeric_limits<double>::infinity();

    auto spec = fromjson("{$convert: {input: 'Infinity', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: 'INF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: 'infinity', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '+InFiNiTy', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '-Infinity', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), -infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '-INF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), -infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '-InFiNiTy', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), -infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '-inf', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), -infValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: '-infinity', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), -infValue, BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertZeroStringsToDouble) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '-0', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDouble);
    ASSERT_TRUE(std::signbit(result.getDouble()));

    spec = fromjson("{$convert: {input: '-0.0', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDouble);
    ASSERT_TRUE(std::signbit(result.getDouble()));

    spec = fromjson("{$convert: {input: '+0', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDouble);
    ASSERT_FALSE(std::signbit(result.getDouble()));

    spec = fromjson("{$convert: {input: '+0.0', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDouble);
    ASSERT_FALSE(std::signbit(result.getDouble()));
}

TEST_F(EvaluateConvertTest, ConvertNanStringsToDouble) {
    auto expCtx = getExpCtx();
    auto nanValue = std::numeric_limits<double>::quiet_NaN();

    auto spec = fromjson("{$convert: {input: 'nan', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_TRUE(std::isnan(result.getDouble()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), nanValue, BSONType::numberDouble);

    spec = fromjson("{$convert: {input: 'Nan', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_TRUE(std::isnan(result.getDouble()));

    spec = fromjson("{$convert: {input: 'NaN', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_TRUE(std::isnan(result.getDouble()));

    spec = fromjson("{$convert: {input: '-NAN', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_TRUE(std::isnan(result.getDouble()));

    spec = fromjson("{$convert: {input: '+NaN', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_TRUE(std::isnan(result.getDouble()));
}

TEST_F(EvaluateConvertTest, ConvertStringToDoubleOverflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '" + kDoubleOverflow.toString() + "', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Out of range");
                             });

    spec =
        fromjson("{$convert: {input: '" + kDoubleNegativeOverflow.toString() + "', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(), "Out of range");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToDoubleUnderflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '1E-1000', to: 'double'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "Failed to parse number '1E-1000' in $convert with no onError value: Out of range");
        });

    spec = fromjson("{$convert: {input: '-1E-1000', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Failed to parse number '-1E-1000' in $convert with no onError "
                                   "value: Out of range");
        });
}

TEST_F(EvaluateConvertTest, ConvertStringToDoubleWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec = fromjson("{$convert: {input: '" + kDoubleOverflow.toString() +
                         "', to: 'double', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '" + kDoubleNegativeOverflow.toString() +
                    "', to: 'double', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '.5.', to: 'double', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '5.5f', to: 'double', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertStringToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '5', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), 5, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '2.02', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), Decimal128("2.02"), BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '2.02E200', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}, &expCtx->variables),
                                   Decimal128("2.02E200"),
                                   BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '" + Decimal128::kLargestPositive.toString() +
                    "', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}, &expCtx->variables),
                                   Decimal128::kLargestPositive,
                                   BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '" + Decimal128::kLargestNegative.toString() +
                    "', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}, &expCtx->variables),
                                   Decimal128::kLargestNegative,
                                   BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertInfinityStringsToDecimal) {
    auto expCtx = getExpCtx();
    auto infValue = Decimal128::kPositiveInfinity;
    auto negInfValue = Decimal128::kNegativeInfinity;

    auto spec = fromjson("{$convert: {input: 'Infinity', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: 'INF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: 'infinity', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '+InFiNiTy', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), infValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-Infinity', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negInfValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-INF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negInfValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-InFiNiTy', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negInfValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-inf', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negInfValue, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-infinity', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negInfValue, BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertNanStringsToDecimal) {
    auto expCtx = getExpCtx();
    auto positiveNan = Decimal128::kPositiveNaN;
    auto negativeNan = Decimal128::kNegativeNaN;

    auto spec = fromjson("{$convert: {input: 'nan', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), positiveNan, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: 'Nan', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), positiveNan, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: 'NaN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), positiveNan, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '+NaN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), positiveNan, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-NAN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negativeNan, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-nan', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negativeNan, BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '-NaN', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), negativeNan, BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertZeroStringsToDecimal) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '-0', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_TRUE(result.getDecimal().isNegative());

    spec = fromjson("{$convert: {input: '-0.0', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_TRUE(result.getDecimal().isNegative());

    spec = fromjson("{$convert: {input: '+0', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_FALSE(result.getDecimal().isNegative());

    spec = fromjson("{$convert: {input: '+0.0', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 0, BSONType::numberDecimal);
    ASSERT_TRUE(result.getDecimal().isZero());
    ASSERT_FALSE(result.getDecimal().isNegative());
}

TEST_F(EvaluateConvertTest, ConvertStringToDecimalOverflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '1E6145', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Conversion from string to decimal would overflow");
                             });

    spec = fromjson("{$convert: {input: '-1E6145', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Conversion from string to decimal would overflow");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToDecimalUnderflow) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '1E-6178', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Conversion from string to decimal would underflow");
                             });

    spec = fromjson("{$convert: {input: '-1E-6177', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "Conversion from string to decimal would underflow");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToDecimalWithPrecisionLoss) {
    auto expCtx = getExpCtx();

    auto spec =
        fromjson("{$convert: {input: '10000000000000000000000000000000001', to: 'decimal'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), Decimal128("1e34"), BSONType::numberDecimal);

    spec = fromjson("{$convert: {input: '1.1250000000000000000000000000000001', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), Decimal128("1.125"), BSONType::numberDecimal);
}

TEST_F(EvaluateConvertTest, ConvertStringToDecimalWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec =
        fromjson("{$convert: {input: '1E6145', to: 'decimal', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec =
        fromjson("{$convert: {input: '-1E-6177', to: 'decimal', onError: '" + onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertStringToNumberFailsForHexStrings) {
    auto expCtx = getExpCtx();
    auto invalidHexFailure = [](const AssertionException& exception) {
        ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
        ASSERT_STRING_CONTAINS(exception.reason(), "Illegal hexadecimal input in $convert");
    };

    auto spec = fromjson("{$convert: {input: '0xFF', to: 'int'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0xFF', to: 'long'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0xFF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0xFF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0x00', to: 'int'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: '0x00', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(
        convertExp->evaluate({}, &expCtx->variables), AssertionException, invalidHexFailure);

    spec = fromjson("{$convert: {input: 'FF', to: 'double'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Did not consume any digits");
                             });

    spec = fromjson("{$convert: {input: 'FF', to: 'decimal'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Failed to parse string to decimal");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToOID) {
    auto expCtx = getExpCtx();
    auto oid = OID::gen();

    auto spec = fromjson("{$convert: {input: '" + oid.toString() + "', to: 'objectId'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), oid, BSONType::oid);

    spec = fromjson("{$convert: {input: '123456789abcdef123456789', to: 'objectId'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate({}, &expCtx->variables),
                                   OID("123456789abcdef123456789"),
                                   BSONType::oid);
}

TEST_F(EvaluateConvertTest, ConvertStringToOIDFailsForInvalidHexStrings) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: 'InvalidHexButSizeCorrect', to: 'objectId'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Invalid character found in hex string");
                             });

    spec = fromjson("{$convert: {input: 'InvalidSize', to: 'objectId'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Invalid string length for parsing to OID");
                             });

    spec = fromjson("{$convert: {input: '0x123456789abcdef123456789', to: 'objectId'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_WITH_CHECK(convertExp->evaluate({}, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_EQ(exception.code(), ErrorCodes::ConversionFailure);
                                 ASSERT_STRING_CONTAINS(exception.reason(),
                                                        "Invalid string length for parsing to OID");
                             });
}

TEST_F(EvaluateConvertTest, ConvertStringToOIDWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "><(((((>"_sd;

    auto spec =
        fromjson("{$convert: {input: 'InvalidHexButSizeCorrect', to: 'objectId', onError: '" +
                 onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: 'InvalidSize', to: 'objectId', onError: '" + onErrorValue +
                    "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);

    spec = fromjson("{$convert: {input: '0x123456789abcdef123456789', to: 'objectId', onError: '" +
                    onErrorValue + "'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate({}, &expCtx->variables), onErrorValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertStringToDateRejectsUnparsableString) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '60.Monday1770/06:59', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = fromjson("{$convert: {input: 'Definitely not a date', to: 'date'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(EvaluateConvertTest, ConvertStringToDateRejectsTimezoneNameInString) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '2017-07-13T10:02:57 Europe/London', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = fromjson("{$convert: {input: 'July 4, 2017 Europe/London', to: 'date'}}");
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convertExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(EvaluateConvertTest, ConvertStringToDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto result =
        convertExp->evaluate({{"path1", Value("2017-07-06T12:35:37Z"_sd)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-06T12:35:37.000Z", result.toString());

    result =
        convertExp->evaluate({{"path1", Value("2017-07-06T12:35:37.513Z"_sd)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-06T12:35:37.513Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-07-06"_sd)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-06T00:00:00.000Z", result.toString());
}

TEST_F(EvaluateConvertTest, ConvertStringWithTimezoneToDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("2017-07-14T12:02:44.771 GMT+02:00"_sd)}},
                                       &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-14T10:02:44.771Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-07-14T12:02:44.771 A"_sd)}},
                                  &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-14T11:02:44.771Z", result.toString());
}

TEST_F(EvaluateConvertTest, ConvertVerbalStringToDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'date'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("July 4th, 2017"_sd)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-04T00:00:00.000Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("July 4th, 2017 12pm"_sd)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-04T12:00:00.000Z", result.toString());

    result = convertExp->evaluate({{"path1", Value("2017-Jul-04 noon"_sd)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::date);
    ASSERT_EQ("2017-07-04T12:00:00.000Z", result.toString());
}

TEST_F(EvaluateConvertTest, ConvertStringToDateWithOnError) {
    auto expCtx = getExpCtx();
    const auto onErrorValue = "(-_-)"_sd;

    auto spec =
        fromjson("{$convert: {input: '$path1', to: 'date', onError: '" + onErrorValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({{"path1", Value("Not a date"_sd)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onErrorValue, BSONType::string);

    result = convertExp->evaluate({{"path1", Value("60.Monday1770/06:59"_sd)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onErrorValue, BSONType::string);

    result = convertExp->evaluate({{"path1", Value("2017-07-13T10:02:57 Europe/London"_sd)}},
                                  &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onErrorValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, ConvertStringToDateWithOnNull) {
    auto expCtx = getExpCtx();
    const auto onNullValue = "(-_-)"_sd;

    auto spec =
        fromjson("{$convert: {input: '$path1', to: 'date', onNull: '" + onNullValue + "'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto result = convertExp->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onNullValue, BSONType::string);

    result = convertExp->evaluate({{"path1", Value(BSONNULL)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, onNullValue, BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatDouble) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", 0.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(zeroInput, &expCtx->variables), "0"_sd, BSONType::string);

    Document negativeZeroInput{{"path1", -0.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeZeroInput, &expCtx->variables), "-0"_sd, BSONType::string);

    Document positiveIntegerInput{{"path1", 1337.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveIntegerInput, &expCtx->variables),
                                   "1337"_sd,
                                   BSONType::string);

    Document negativeIntegerInput{{"path1", -1337.0}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeIntegerInput, &expCtx->variables),
                                   "-1337"_sd,
                                   BSONType::string);

    Document positiveFractionalInput{{"path1", 0.1337}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveFractionalInput, &expCtx->variables),
        "0.1337"_sd,
        BSONType::string);

    Document negativeFractionalInput{{"path1", -0.1337}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeFractionalInput, &expCtx->variables),
        "-0.1337"_sd,
        BSONType::string);

    Document positiveLargeInput{{"path1", 1.3e37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveLargeInput, &expCtx->variables),
                                   "1.3e+37"_sd,
                                   BSONType::string);

    Document negativeLargeInput{{"path1", -1.3e37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeLargeInput, &expCtx->variables),
                                   "-1.3e+37"_sd,
                                   BSONType::string);

    Document positiveTinyInput{{"path1", 1.3e-37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveTinyInput, &expCtx->variables),
                                   "1.3e-37"_sd,
                                   BSONType::string);

    Document negativeTinyInput{{"path1", -1.3e-37}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeTinyInput, &expCtx->variables),
                                   "-1.3e-37"_sd,
                                   BSONType::string);

    Document infinityInput{{"path1", std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(infinityInput, &expCtx->variables), "Infinity"_sd, BSONType::string);

    Document negativeInfinityInput{{"path1", -std::numeric_limits<double>::infinity()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeInfinityInput, &expCtx->variables),
                                   "-Infinity"_sd,
                                   BSONType::string);

    Document nanInput{{"path1", std::numeric_limits<double>::quiet_NaN()}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nanInput, &expCtx->variables), "NaN"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatObjectId) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", OID("000000000000000000000000")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(zeroInput, &expCtx->variables),
                                   "000000000000000000000000"_sd,
                                   BSONType::string);

    Document simpleInput{{"path1", OID("0123456789abcdef01234567")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(simpleInput, &expCtx->variables),
                                   "0123456789abcdef01234567"_sd,
                                   BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatBool) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document trueInput{{"path1", true}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(trueInput, &expCtx->variables), "true"_sd, BSONType::string);

    Document falseInput{{"path1", false}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(falseInput, &expCtx->variables), "false"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatDate) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document epochInput{{"path1", Date_t::fromMillisSinceEpoch(0LL)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(epochInput, &expCtx->variables),
                                   "1970-01-01T00:00:00.000Z"_sd,
                                   BSONType::string);

    Document dateInput{{"path1", Date_t::fromMillisSinceEpoch(872835240000)}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(dateInput, &expCtx->variables),
                                   "1997-08-29T06:14:00.000Z"_sd,
                                   BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatInt) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", int{0}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(zeroInput, &expCtx->variables), "0"_sd, BSONType::string);

    Document positiveInput{{"path1", int{1337}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveInput, &expCtx->variables), "1337"_sd, BSONType::string);

    Document negativeInput{{"path1", int{-1337}}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeInput, &expCtx->variables), "-1337"_sd, BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatLong) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", 0LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(zeroInput, &expCtx->variables), "0"_sd, BSONType::string);

    Document positiveInput{{"path1", 1337133713371337LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveInput, &expCtx->variables),
                                   "1337133713371337"_sd,
                                   BSONType::string);

    Document negativeInput{{"path1", -1337133713371337LL}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeInput, &expCtx->variables),
                                   "-1337133713371337"_sd,
                                   BSONType::string);
}

TEST_F(EvaluateConvertTest, FormatDecimal) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document zeroInput{{"path1", Decimal128("0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(zeroInput, &expCtx->variables), "0"_sd, BSONType::string);

    Document negativeZeroInput{{"path1", Decimal128("-0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeZeroInput, &expCtx->variables), "-0"_sd, BSONType::string);

    Document preciseZeroInput{{"path1", Decimal128("0.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(preciseZeroInput, &expCtx->variables), "0.0"_sd, BSONType::string);

    Document negativePreciseZeroInput{{"path1", Decimal128("-0.0")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativePreciseZeroInput, &expCtx->variables),
        "-0.0"_sd,
        BSONType::string);

    Document extraPreciseZeroInput{{"path1", Decimal128("0.0000")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(extraPreciseZeroInput, &expCtx->variables),
                                   "0.0000"_sd,
                                   BSONType::string);

    Document positiveIntegerInput{{"path1", Decimal128("1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveIntegerInput, &expCtx->variables),
                                   "1337"_sd,
                                   BSONType::string);

    Document largeIntegerInput{{"path1", Decimal128("13370000000000000000000000000000000")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(largeIntegerInput, &expCtx->variables),
                                   "1.337000000000000000000000000000000E+34"_sd,
                                   BSONType::string);

    Document negativeIntegerInput{{"path1", Decimal128("-1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeIntegerInput, &expCtx->variables),
                                   "-1337"_sd,
                                   BSONType::string);

    Document positiveFractionalInput{{"path1", Decimal128("0.1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positiveFractionalInput, &expCtx->variables),
        "0.1337"_sd,
        BSONType::string);

    Document positivePreciseFractionalInput{{"path1", Decimal128("0.133700")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(positivePreciseFractionalInput, &expCtx->variables),
        "0.133700"_sd,
        BSONType::string);

    Document negativeFractionalInput{{"path1", Decimal128("-0.1337")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeFractionalInput, &expCtx->variables),
        "-0.1337"_sd,
        BSONType::string);

    Document negativePreciseFractionalInput{{"path1", Decimal128("-0.133700")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativePreciseFractionalInput, &expCtx->variables),
        "-0.133700"_sd,
        BSONType::string);

    Document positiveLargeInput{{"path1", Decimal128("1.3e37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveLargeInput, &expCtx->variables),
                                   "1.3E+37"_sd,
                                   BSONType::string);

    Document negativeLargeInput{{"path1", Decimal128("-1.3e37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeLargeInput, &expCtx->variables),
                                   "-1.3E+37"_sd,
                                   BSONType::string);

    Document positiveTinyInput{{"path1", Decimal128("1.3e-37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(positiveTinyInput, &expCtx->variables),
                                   "1.3E-37"_sd,
                                   BSONType::string);

    Document negativeTinyInput{{"path1", Decimal128("-1.3e-37")}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeTinyInput, &expCtx->variables),
                                   "-1.3E-37"_sd,
                                   BSONType::string);

    Document infinityInput{{"path1", Decimal128::kPositiveInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(infinityInput, &expCtx->variables), "Infinity"_sd, BSONType::string);

    Document negativeInfinityInput{{"path1", Decimal128::kNegativeInfinity}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(convertExp->evaluate(negativeInfinityInput, &expCtx->variables),
                                   "-Infinity"_sd,
                                   BSONType::string);

    Document nanInput{{"path1", Decimal128::kPositiveNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(nanInput, &expCtx->variables), "NaN"_sd, BSONType::string);

    Document negativeNaNInput{{"path1", Decimal128::kNegativeNaN}};
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convertExp->evaluate(negativeNaNInput, &expCtx->variables), "NaN"_sd, BSONType::string);
}

Value runConvertBinDataToNumeric(boost::intrusive_ptr<ExpressionContextForTest> expCtx,
                                 BSONObj spec,
                                 std::vector<unsigned char> valueBytes) {
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    BSONBinData inputBinData{
        valueBytes.data(), static_cast<int>(valueBytes.size()), BinDataGeneral};
    return convertExp->evaluate({{"path1", Value(inputBinData)}}, &expCtx->variables);
}

template <typename T>
void testConvertNumericToBinData(boost::intrusive_ptr<ExpressionContextForTest> expCtx,
                                 BSONObj spec,
                                 std::vector<unsigned char> valueBytes,
                                 T inputVal) {
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    BSONBinData expectedBinData{
        valueBytes.data(), static_cast<int>(valueBytes.size()), BinDataGeneral};
    auto result = convertExp->evaluate({{"path1", Value(inputVal)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, expectedBinData, BSONType::binData);
}


TEST_F(EvaluateConvertTest, ConvertBinDataToIntFourBytesBigEndian) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'int', byteOrder: 'big'}}"),
        {0b00000000, 0b00000000, 0b00000000, 0b00000010});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 2, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToIntHexBigEndian) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'int', byteOrder: 'big'}}"),
        {0x00, 0x01, 0x00, 0x00});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 65536, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToIntOneByteBigEndian) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'int', byteOrder: 'big'}}"),
        {0b00000010});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 2, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToIntTwoByteLittleEndian) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'int', byteOrder: 'little'}}"),
        {0b00000001, 0b00000000});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 1, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToIntTwoByteDefaultEndianness) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx, fromjson("{$convert: {input: '$path1', to: 'int'}}"), {0b00000001, 0b00000000});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 1, BSONType::numberInt);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToIntWrongByteLength) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'int', byteOrder: 'big'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::vector<int8_t> intBytes = {0b00000010, 0b00000010, 0b00000010};
    BSONBinData inputBinData{intBytes.data(), static_cast<int>(intBytes.size()), BinDataGeneral};

    ASSERT_THROWS_CODE(convertExp->evaluate({{"path1", Value(inputBinData)}}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(EvaluateConvertTest, ConvertIntToBinLargeNegative) {
    auto expCtx = getExpCtx();
    int intVal = -300;
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}"),
        {0b11111111, 0b11111111, 0b11111110, 0b11010100},
        intVal);
}

TEST_F(EvaluateConvertTest, ConvertToBinDataFromToIntInput) {
    auto expCtx = getExpCtx();
    std::string input = "1";
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: {$toInt: '$path1'}, to: 'binData', byteOrder: 'big'}}"),
        {0x00, 0x00, 0x00, 0x01},
        input);
}

TEST_F(EvaluateConvertTest, ConvertIntToBinDataLittleEndian) {
    auto expCtx = getExpCtx();
    int intVal = -2;
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'little'}}"),
        {0b11111110, 0b11111111, 0b11111111, 0b11111111},
        intVal);
}

TEST_F(EvaluateConvertTest, ConvertIntToBinDataNoDefaultFails) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'binData'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    std::vector<unsigned char> longBytes = {0b11001101,
                                            0b00000000,
                                            0b00000000,
                                            0b00000000,
                                            0b00000000,
                                            0b00000000,
                                            0b00000000,
                                            0b00000000};
    BSONBinData expectedBinData{
        longBytes.data(), static_cast<int>(longBytes.size()), BinDataGeneral};
    long long longVal = 205;

    auto result = convertExp->evaluate({{"path1", Value(longVal)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, expectedBinData, BSONType::binData);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToLongLittleEndian) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'long', byteOrder: 'little'}}"),
        {0b01000000, 0b00000000, 0b00000000, 0b00000000});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 64, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToLongBigEndian) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'long', byteOrder: 'big'}}"),
        {0b01000000, 0b00000000});
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 16384, BSONType::numberLong);
}

TEST_F(EvaluateConvertTest, ConvertLongToBinDataBigEndian) {
    auto expCtx = getExpCtx();
    long long longVal = 205;
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}"),
        {0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b11001101},
        longVal);
}

TEST_F(EvaluateConvertTest, ConvertToBinDataFromToLongInput) {
    auto expCtx = getExpCtx();
    std::string input = "75";
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: {$toLong: '$path1'}, to: 'binData', byteOrder: 'big'}}"),
        {0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b1001011},
        input);
}

TEST_F(EvaluateConvertTest, ConvertLongToBinDataBadByteOrderFails) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'weird'}}");

    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    int val = 0;

    ASSERT_THROWS_CODE(convertExp->evaluate({{"path1", Value(val)}}, &expCtx->variables),
                       AssertionException,
                       9130002);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleBigEndianSinglePrecision) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'big'}}"),
        {0b01000001, 0b01010011, 0b00110011, 0b00110011});
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(result.getDouble(), 13.2, 0.001);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleLittleEndianSinglePrecision) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}"),
        {0x66, 0xe6, 0x40, 0xc6});
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(result.getDouble(), -12345.6, 0.001);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleBigEndianDoublePrecision) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'big'}}"),
        {0x40, 0x6B, 0xC7, 0x0A, 0x3D, 0x70, 0xA3, 0xD7});
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(result.getDouble(), 222.22, 0.001);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleLittleEndianDoublePrecision) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}"),
        {0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b11100000,
         0b10111111});
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(result.getDouble(), -0.5, 0.001);
}

TEST_F(EvaluateConvertTest, ConvertDoubleToBinDataBigEndian) {
    auto expCtx = getExpCtx();
    double doubleVal = -2.5;
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}"),
        {0b11000000,
         0b00000100,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000},
        doubleVal);
}

TEST_F(EvaluateConvertTest, ConvertDoubleToBinDataLittleEndian) {
    auto expCtx = getExpCtx();
    double doubleVal = 178.0;
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'little'}}"),
        {0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b00000000,
         0b01000000,
         0b01100110,
         0b01000000},
        doubleVal);
}

TEST_F(EvaluateConvertTest, ConvertDoubleToBinDataBigEndianNegativeInf) {
    auto expCtx = getExpCtx();
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}"),
        {0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        -std::numeric_limits<double>::infinity());
}

TEST_F(EvaluateConvertTest, ConvertDoubleToBinDataLittleEndianInf) {
    auto expCtx = getExpCtx();
    testConvertNumericToBinData(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'little'}}"),
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x7F},
        std::numeric_limits<double>::infinity());
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleBigEndianSinglePrecisionInf) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'big'}}"),
        {0x7F, 0x80, 0x00, 0x00});
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        result, std::numeric_limits<double>::infinity(), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleBigEndianDoublePrecisionInf) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'big'}}"),
        {0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        result, std::numeric_limits<double>::infinity(), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleLittleEndianSinglePrecisionNegativeInf) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}"),
        {0x00, 0x00, 0x80, 0xFF});
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        result, -std::numeric_limits<double>::infinity(), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleLittleEndianDoublePrecisionNegativeInf) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}"),
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xFF});
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        result, -std::numeric_limits<double>::infinity(), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleBigEndianSinglePrecisionQuietNan) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'big'}}"),
        {0x7F, 0x80, 0x00, 0x02});
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        result, std::numeric_limits<double>::quiet_NaN(), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleBigEndianSinglePrecisionSignalingNan) {
    auto expCtx = getExpCtx();
    auto result = runConvertBinDataToNumeric(
        expCtx,
        fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}"),
        {0x05, 0x00, 0x80, 0x7F});
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        result, std::numeric_limits<double>::signaling_NaN(), BSONType::numberDouble);
}

TEST_F(EvaluateConvertTest, ConvertBinDataToDoubleWrongLengthFails) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    std::vector<unsigned char> doubleBytes = {0x05, 0x00, 0x80};
    BSONBinData inputBinData{
        doubleBytes.data(), static_cast<int>(doubleBytes.size()), BinDataGeneral};

    Document binDataInfinity{{"path1", inputBinData}};

    ASSERT_THROWS_CODE(convertExp->evaluate(binDataInfinity, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

void assertBinaryInRange(BSONBinData actual,
                         std::vector<unsigned char> lowerBound0,
                         std::vector<unsigned char> upperBound0,
                         std::vector<unsigned char> lowerBound1,
                         std::vector<unsigned char> upperBound1) {
    ASSERT_EQ(actual.length, lowerBound0.size());
    ASSERT_EQ(actual.length, upperBound0.size());
    ASSERT_EQ(actual.length, lowerBound1.size());
    ASSERT_EQ(actual.length, upperBound1.size());

    bool firstRange = (memcmp(lowerBound0.data(), actual.data, actual.length) <= 0) &&
        (memcmp(actual.data, upperBound0.data(), actual.length) <= 0);
    bool secondRange = (memcmp(lowerBound1.data(), actual.data, actual.length) <= 0) &&
        (memcmp(actual.data, upperBound1.data(), actual.length) <= 0);

    ASSERT(firstRange || secondRange);
}

// For signaling and quiet NaNs there is a range the binary can fall within. The following tests
// confirm that the resulting BinData is within that range.
TEST_F(EvaluateConvertTest, ConvertDoubleToBinDataSignalingNan) {
    auto expCtx = getExpCtx();
    auto convertExp = Expression::parseExpression(
        expCtx.get(),
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}"),
        expCtx->variablesParseState);
    auto result = convertExp->evaluate(
        {{"path1", Value(std::numeric_limits<double>::signaling_NaN())}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::binData);
    assertBinaryInRange(result.getBinData(),
                        {0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
                        {0x7F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                        {0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
                        {0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
}

TEST_F(EvaluateConvertTest, ConvertDoubleToBinDataQuietNan) {
    auto expCtx = getExpCtx();
    auto convertExp = Expression::parseExpression(
        expCtx.get(),
        fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}"),
        expCtx->variablesParseState);
    auto result = convertExp->evaluate({{"path1", Value(std::numeric_limits<double>::quiet_NaN())}},
                                       &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::binData);
    assertBinaryInRange(result.getBinData(),
                        {0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                        {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                        {0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
}


}  // namespace evaluate_convert_test

namespace evaluate_convert_shortcut_test {

using EvaluateConvertShortcutTest = AggregationContextFixture;

TEST_F(EvaluateConvertShortcutTest, AcceptsSingleArgumentInArrayOrByItself) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << "1");
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));

    spec = BSON("$toInt" << BSON_ARRAY("1"));
    convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));

    spec = BSON("$toInt" << BSON_ARRAY(BSON_ARRAY("1")));
    convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(convert->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToInts) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << "1");
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(1), BSONType::numberInt);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsBinDataToInts) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convertExp.get()));

    std::vector<int8_t> intBytes = {0b00000001, 0b00000000};
    BSONBinData inputBinData{intBytes.data(), static_cast<int>(intBytes.size()), bdtUUID};

    auto result = convertExp->evaluate({{"path1", Value(inputBinData)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 1, BSONType::numberInt);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsBinDataToLongs) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toLong" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convertExp.get()));

    std::vector<int8_t> intBytes = {0b00000001, 0b00000000};
    BSONBinData inputBinData{intBytes.data(), static_cast<int>(intBytes.size()), BinDataGeneral};

    auto result = convertExp->evaluate({{"path1", Value(inputBinData)}}, &expCtx->variables);
    ASSERT_VALUE_CONTENTS_AND_TYPE(result, 1, BSONType::numberLong);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsBinDataToDoubles) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDouble" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convertExp.get()));

    std::vector<unsigned char> intBytes = {0b00000000,
                                           0b00000000,
                                           0b00000000,
                                           0b00000000,
                                           0b00000000,
                                           0b00000000,
                                           0b11100000,
                                           0b10111111};
    BSONBinData inputBinData{intBytes.data(), static_cast<int>(intBytes.size()), BinDataGeneral};

    auto result = convertExp->evaluate({{"path1", Value(inputBinData)}}, &expCtx->variables);
    ASSERT_EQ(result.getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(result.getDouble(), -0.5, 0.001);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToLongs) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toLong" << "1");
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(1), BSONType::numberLong);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToDoubles) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDouble" << "1");
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(1), BSONType::numberDouble);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToDecimals) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDecimal" << "1");
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(1), BSONType::numberDecimal);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToDates) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toDate" << 0LL);
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}, &expCtx->variables),
                                   Value(Date_t::fromMillisSinceEpoch(0)),
                                   BSONType::date);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToObjectIds) {
    auto expCtx = getExpCtx();

    const auto hexString = "deadbeefdeadbeefdeadbeef"_sd;
    BSONObj spec = BSON("$toObjectId" << hexString);
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(convert->evaluate({}, &expCtx->variables),
                                   Value(OID::createFromString(hexString)),
                                   BSONType::oid);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToString) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toString" << 1);
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value("1"_sd), BSONType::string);
}

TEST_F(EvaluateConvertShortcutTest, ConvertsToBool) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toBool" << 1);
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(true), BSONType::boolean);
}

TEST_F(EvaluateConvertShortcutTest, ReturnsNullOnNullishInput) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toBool" << BSONNULL);
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(BSONNULL), BSONType::null);

    spec = BSON("$toInt" << "$missing");
    convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_VALUE_CONTENTS_AND_TYPE(
        convert->evaluate({}, &expCtx->variables), Value(BSONNULL), BSONType::null);
}

TEST_F(EvaluateConvertShortcutTest, ThrowsOnConversionFailure) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << "not an int");
    auto convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_THROWS_CODE(convert->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = BSON("$toObjectId" << "not all hex values");
    convert = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionConvert*>(convert.get()));
    ASSERT_THROWS_CODE(convert->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

}  // namespace evaluate_convert_shortcut_test
}  // namespace mongo
