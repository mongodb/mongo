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

namespace ExpressionConvertTest {

using ExpressionConvertTest = AggregationContextFixture;

TEST_F(ExpressionConvertTest, ParseAndSerializeWithoutOptionalArguments) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}}}")),
                    convertExp->serialize());

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}}}")),
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

TEST_F(ExpressionConvertTest, ParseAndSerializeWithToSubDocument) {
    auto expCtx = getExpCtx();

    auto spec =
        BSON("$convert" << BSON("input" << "$path1"
                                        << "to"
                                        << BSON("type" << "binData"
                                                       << "subtype" << static_cast<int>(newUUID))
                                        << "format"
                                        << "uuid"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(Value(fromjson(  // NOLINT
                        R"({
                            $convert: {
                                input: '$path1', 
                                to: {
                                    type: {$const: 'binData'},
                                    subtype: {$const: 4}
                                },
                                format: {$const: 'uuid'}
                            }
                        })")),
                    convertExp->serialize());

    ASSERT_VALUE_EQ(
        Value(fromjson(  // NOLINT
            R"({
                $convert: {
                    input: '$path1', 
                    to: {
                        type: {$const: 'binData'},
                        subtype: {$const: 4}
                    },
                    format: {$const: 'uuid'}
                }
            })")),
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));

    ASSERT_VALUE_EQ(
        Value(fromjson(  // NOLINT
            R"({
                $convert: {
                    input: '$path1', 
                    to: {$const: {"?": "?"}},
                    format: {$const: "?"}
                }
            })")),
        convertExp->serialize(SerializationOptions::kRepresentativeQueryShapeSerializeOptions));

    ASSERT_VALUE_EQ(Value(fromjson(  // NOLINT
                        R"({
                            $convert: {
                                input: '$path1', 
                                to: "?object",
                                format: "?string"
                            }
                        })")),
                    convertExp->serialize(SerializationOptions::kDebugQueryShapeSerializeOptions));
}

TEST_F(ExpressionConvertTest, ParseAndSerializeWithOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "onError" << 0));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onError: {$const: 0}}}")),
        convertExp->serialize());

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onError: {$const: 0}}}")),
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

TEST_F(ExpressionConvertTest, ParseAndSerializeWithOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "onNull" << 0));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onNull: {$const: 0}}}")),
        convertExp->serialize());

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, onNull: {$const: 0}}}")),
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

TEST_F(ExpressionConvertTest, ParseAndSerializeWithBase) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "to"
                                                << "int"
                                                << "base" << 8));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, base: {$const: 8}}}")),
        convertExp->serialize());

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, base: {$const: 8}}}")),
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));

    spec = BSON("$convert" << BSON("input" << "$path1"
                                           << "to"
                                           << "int"
                                           << "base" << "$path2"));
    convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, base: '$path2'}}")),
        convertExp->serialize());

    ASSERT_VALUE_EQ(
        Value(fromjson("{$convert: {input: '$path1', to: {$const: 'int'}, base: '$path2'}}")),
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

TEST_F(ExpressionConvertTest, ConvertWithoutInputFailsToParse) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("to" << "int"
                                             << "onError" << 0));
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(exception.reason(), "Missing 'input' parameter to $convert");
        });
}

TEST_F(ExpressionConvertTest, ConvertWithoutToFailsToParse) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << "$path1"
                                                << "onError" << 0));
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(exception.reason(), "Missing 'to' parameter to $convert");
        });
}

TEST_F(ExpressionConvertTest, RoundTripSerialization) {
    auto expCtx = getExpCtx();

    // Round-trip serialization of an argument that *looks* like an expression.
    auto spec =
        BSON("$convert" << BSON(
                 "input" << BSON("$literal" << BSON("$toString" << "this is a string")) << "to"
                         << "string"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto opts = SerializationOptions{LiteralSerializationPolicy::kToRepresentativeParseableValue};
    auto serialized = convertExp->serialize(opts);
    ASSERT_VALUE_EQ(Value(BSON("$convert" << BSON("input" << BSON("$const" << BSON("?" << "?"))
                                                          << "to" << BSON("$const" << "string")))),
                    serialized);

    auto roundTrip = Expression::parseExpression(expCtx.get(),
                                                 serialized.getDocument().toBson(),
                                                 expCtx->variablesParseState)
                         ->serialize(opts);
    ASSERT_VALUE_EQ(roundTrip, serialized);
}

TEST_F(ExpressionConvertTest, ConvertOptimizesToExpressionConstant) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << 0 << "to"
                                                << "double"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    convertExp = convertExp->optimize();

    auto constResult = dynamic_cast<ExpressionConstant*>(convertExp.get());
    ASSERT(constResult);
    ASSERT_VALUE_CONTENTS_AND_TYPE(constResult->getValue(), 0.0, BSONType::numberDouble);
}

TEST_F(ExpressionConvertTest, ConvertWithFormatOptimizesToExpressionConstant) {
    auto expCtx = getExpCtx();

    std::string inputStr{"123"};

    auto spec =
        BSON("$convert" << BSON("input" << base64::encode(inputStr) << "to"
                                        << "binData"
                                        << "format" << toStringData(BinDataFormat::kBase64)));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    convertExp = convertExp->optimize();

    BSONBinData result{inputStr.data(), static_cast<int>(inputStr.size()), BinDataGeneral};

    auto constResult = dynamic_cast<ExpressionConstant*>(convertExp.get());
    ASSERT(constResult);
    ASSERT_VALUE_CONTENTS_AND_TYPE(constResult->getValue(), result, BSONType::binData);
}

TEST_F(ExpressionConvertTest, ConvertWithOnErrorOptimizesToExpressionConstant) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << 0 << "to"
                                                << "objectId"
                                                << "onError"
                                                << "X"));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    convertExp = convertExp->optimize();

    auto constResult = dynamic_cast<ExpressionConstant*>(convertExp.get());
    ASSERT(constResult);
    ASSERT_VALUE_CONTENTS_AND_TYPE(constResult->getValue(), "X"_sd, BSONType::string);
}

TEST_F(ExpressionConvertTest, ConvertWithBaseOptimizesToExpressionConstant) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$convert" << BSON("input" << 160 << "to"
                                                << "string"
                                                << "base" << 16));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    convertExp = convertExp->optimize();

    Value result{"A0"_sd};

    auto constResult = dynamic_cast<ExpressionConstant*>(convertExp.get());
    ASSERT(constResult);
    ASSERT_VALUE_CONTENTS_AND_TYPE(constResult->getValue(), result, BSONType::string);
}

TEST_F(ExpressionConvertTest, ConvertBinDataToIntFeatureFlagOffFails) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBinDataConvertNumeric",
                                                               false);

    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'int', byteOrder: 'little'}}");

    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionConvertTest, ConvertIntToBindataFeatureFlagOffFails) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBinDataConvertNumeric",
                                                               false);
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'little'}}");

    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionConvertTest, ConvertBinDataToLongFeatureFlagOffFails) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBinDataConvertNumeric",
                                                               false);
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'long', byteOrder: 'little'}}");

    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionConvertTest, ConvertLongToBinDataFeatureFlagOffFails) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBinDataConvertNumeric",
                                                               false);
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'binData', byteOrder: 'big'}}");

    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionConvertTest, ConvertBinDataToDoubleFeatureFlagOffFails) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBinDataConvertNumeric",
                                                               false);
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$convert: {input: '$path1', to: 'double', byteOrder: 'little'}}");

    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

}  // namespace ExpressionConvertTest

namespace ExpressionConvertShortcutsTest {

using ExpressionConvertShortcutsTest = AggregationContextFixture;

TEST_F(ExpressionConvertShortcutsTest, RejectsMoreThanOneInput) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << BSON_ARRAY(1 << 3));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toLong" << BSON_ARRAY(1 << 3));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toDouble" << BSON_ARRAY(1 << 3));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
}

TEST_F(ExpressionConvertShortcutsTest, RejectsZeroInputs) {
    auto expCtx = getExpCtx();

    BSONObj spec = BSON("$toInt" << BSONArray());
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toLong" << BSONArray());
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
    spec = BSON("$toDouble" << BSONArray());
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       50723);
}

}  // namespace ExpressionConvertShortcutsTest
}  // namespace mongo
