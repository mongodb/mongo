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

#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

TEST(MatchExpressionParserTest, MinDistanceWithoutNearFailsToParse) {
    BSONObj query = fromjson("{loc: {$minDistance: 10}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

StatusWith<int> fib(int n) {
    if (n < 0)
        return StatusWith<int>(ErrorCodes::BadValue, "paramter to fib has to be >= 0");
    if (n <= 1)
        return StatusWith<int>(1);
    StatusWith<int> a = fib(n - 1);
    StatusWith<int> b = fib(n - 2);
    if (!a.isOK())
        return a;
    if (!b.isOK())
        return b;
    return StatusWith<int>(a.getValue() + b.getValue());
}

TEST(StatusWithTest, Fib1) {
    StatusWith<int> x = fib(-2);
    ASSERT(!x.isOK());

    x = fib(0);
    ASSERT(x.isOK());
    ASSERT(1 == x.getValue());

    x = fib(1);
    ASSERT(x.isOK());
    ASSERT(1 == x.getValue());

    x = fib(2);
    ASSERT(x.isOK());
    ASSERT(2 == x.getValue());

    x = fib(3);
    ASSERT(x.isOK());
    ASSERT(3 == x.getValue());
}

TEST(MatchExpressionParserTest, AlwaysFalseFailsToParseNonOneArguments) {
    auto queryIntArgument = BSON(AlwaysFalseMatchExpression::kName << 0);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(queryIntArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON(AlwaysFalseMatchExpression::kName << "");
    expr = MatchExpressionParser::parse(queryStringArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON(AlwaysFalseMatchExpression::kName << 1.1);
    expr = MatchExpressionParser::parse(queryDoubleArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON(AlwaysFalseMatchExpression::kName << true);
    expr = MatchExpressionParser::parse(queryFalseArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserTest, AlwaysTrueFailsToParseNonOneArguments) {
    auto queryIntArgument = BSON(AlwaysTrueMatchExpression::kName << 0);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(queryIntArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON(AlwaysTrueMatchExpression::kName << "");
    expr = MatchExpressionParser::parse(queryStringArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON(AlwaysTrueMatchExpression::kName << 1.1);
    expr = MatchExpressionParser::parse(queryDoubleArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON(AlwaysTrueMatchExpression::kName << true);
    expr = MatchExpressionParser::parse(queryFalseArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserTest, TextFailsToParseWhenDisallowed) {
    auto query = fromjson("{$text: {$search: 'str'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, TextParsesSuccessfullyWhenAllowed) {
    auto query = fromjson("{$text: {$search: 'str'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kText)
            .getStatus());
}

TEST(MatchExpressionParserTest, TextFailsToParseIfNotTopLevel) {
    auto query = fromjson("{a: {$text: {$search: 'str'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kText)
            .getStatus());
}

TEST(MatchExpressionParserTest, TextWithinElemMatchFailsToParse) {
    auto query = fromjson("{a: {$elemMatch: {$text: {$search: 'str'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kText)
            .getStatus());

    query = fromjson("{a: {$elemMatch: {$elemMatch: {$text: {$search: 'str'}}}}}");
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kText)
            .getStatus());
}

TEST(MatchExpressionParserTest, WhereFailsToParseWhenDisallowed) {
    auto query = fromjson("{$where: 'this.a == this.b'}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, WhereParsesSuccessfullyWhenAllowed) {
    auto query = fromjson("{$where: 'this.a == this.b'}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query,
                                           expCtx,
                                           ExtensionsCallbackNoop(),
                                           MatchExpressionParser::AllowedFeatures::kJavascript)
                  .getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithoutOptions) {
    auto query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithOptionsInline) {
    auto query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "i")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithoutOptionsInlineAndEmptyOptionsStr) {
    auto query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                           << ""));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexDoesNotParseSuccessfullyWithOptionsInlineAndEmptyOptionsStr) {
    auto query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "i") << "$options"
                                           << ""));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithOptionsNotInline) {
    auto query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                           << "i"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexDoesNotParseSuccessfullyWithMultipleOptions) {
    auto query = BSON("a" << BSON("$options" << "s"
                                             << "$regex" << BSONRegEx("/myRegex/", "i")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithOptionsFirst) {
    auto query = BSON("a" << BSON("$options" << "s"
                                             << "$regex" << BSONRegEx("/myRegex/", "")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithOptionsFirstEmptyOptions) {
    auto query = BSON("a" << BSON("$options" << ""
                                             << "$regex" << BSONRegEx("/myRegex/", "")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}


TEST(MatchExpressionParserTest, NearSphereFailsToParseWhenDisallowed) {
    auto query = fromjson("{a: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, NearSphereParsesSuccessfullyWhenAllowed) {
    auto query = fromjson("{a: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query,
                                           expCtx,
                                           ExtensionsCallbackNoop(),
                                           MatchExpressionParser::AllowedFeatures::kGeoNear)
                  .getStatus());
}

TEST(MatchExpressionParserTest, GeoNearFailsToParseWhenDisallowed) {
    auto query = fromjson("{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, GeoNearParsesSuccessfullyWhenAllowed) {
    auto query = fromjson("{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query,
                                           expCtx,
                                           ExtensionsCallbackNoop(),
                                           MatchExpressionParser::AllowedFeatures::kGeoNear)
                  .getStatus());
}

TEST(MatchExpressionParserTest, NearFailsToParseWhenDisallowed) {
    auto query = fromjson("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, NearParsesSuccessfullyWhenAllowed) {
    auto query = fromjson("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query,
                                           expCtx,
                                           ExtensionsCallbackNoop(),
                                           MatchExpressionParser::AllowedFeatures::kGeoNear)
                  .getStatus());
}

TEST(MatchExpressionParserTest, ExprFailsToParseWhenDisallowed) {
    auto query = fromjson("{$expr: {$eq: ['$a', 5]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kBanAllSpecialFeatures)
            .getStatus());
}

TEST(MatchExpressionParserTest, ExprParsesSuccessfullyWhenAllowed) {
    auto query = fromjson("{$expr: {$eq: ['$a', 5]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, ExprParsesSuccessfullyWithAdditionalTopLevelPredicates) {
    auto query = fromjson("{x: 1, $expr: {$eq: ['$a', 5]}, y: 1}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, ExprParsesSuccessfullyWithinTopLevelOr) {
    auto query = fromjson("{$or: [{x: 1}, {$expr: {$eq: ['$a', 5]}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr)
            .getStatus());
}

TEST(MatchExpressionParserTest, ExprParsesSuccessfullyWithinTopLevelAnd) {
    auto query = fromjson("{$and: [{x: 1}, {$expr: {$eq: ['$a', 5]}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr)
            .getStatus());
}

TEST(MatchExpressionParserTest, ExprParseFailsWithEmptyAnd) {
    auto query = fromjson("{$and: []}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(), "$and argument must be a non-empty array");
}

TEST(MatchExpressionParserTest, ExprParseFailsWithNotArrayAnd) {
    auto query = fromjson("{$and: 'dummy string'}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(), "$and argument must be an array");
}

TEST(MatchExpressionParserTest, ExprParseFailsWithNotObjectInArrayAnd) {
    auto query = fromjson("{$and: ['dummy string']}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(), "$and argument's entries must be objects");
}

TEST(MatchExpressionParserTest, ExprFailsToParseWithTopLevelNot) {
    auto query = fromjson("{$not: {x: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    ASSERT_TRUE(
        result.getStatus().reason() ==
        "unknown top level operator: $not. If you are trying to negate an entire expression, "
        "use $nor.");
}

TEST(MatchExpressionParserTest, ExprParseFailsWithStringNot) {
    auto query = fromjson("{ a: {$not: 'dummy string'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(), "$not argument must be a regex or an object");
}

TEST(MatchExpressionParserTest, ExprParseFailsWithEmptyNot) {
    auto query = fromjson("{ a: {$not: {}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(), "$not argument must be a non-empty object");
}

TEST(MatchExpressionParserTest, ExprFailsToParseWithinElemMatch) {
    auto query = fromjson("{a: {$elemMatch: {$expr: {$eq: ['$foo', '$bar']}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr)
            .getStatus());
}

TEST(MatchExpressionParserTest, ExprNestedFailsToParseWithinElemMatch) {
    auto query =
        fromjson("{a: {$elemMatch: {b: 1, $or: [{$expr: {$eq: ['$foo', '$bar']}}, {c: 1}]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr)
            .getStatus());
}

TEST(MatchExpressionParserTest, ExprFailsToParseWithinInternalSchemaObjectMatch) {
    auto query = fromjson("{a: {$_internalSchemaObjectMatch: {$expr: {$eq: ['$foo', '$bar']}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kExpr)
            .getStatus());
}

TEST(MatchExpressionParserTest, InternalExprEqComparisonToArrayDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = fromjson("{'a.b': {$_internalExprEq: [5]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(MatchExpressionParserTest, InternalExprEqComparisonToUndefinedDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = fromjson("{'a.b': {$_internalExprEq: undefined}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(MatchExpressionParserTest, SampleRateDesugarsToExprAndExpressionRandom) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Test parsing of argument in incements of 0.001.
    for (int i = 0; i <= 1000; i++) {
        BSONObj query = BSON("$sampleRate" << i / 1000.0);
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
    }

    // Does the implicit double conversion work for a large decimal?
    BSONObj query = fromjson("{$sampleRate: 0.999999999999999999999}");
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());
}

TEST(MatchExpressionParserTest, SampleRateFailureCases) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto result = MatchExpressionParser::parse(fromjson("{$sampleRate: -0.25}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: 2.5}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: 2}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: -2}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: {$const: 0.25}}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: NaN}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: inf}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());

    result = MatchExpressionParser::parse(fromjson("{$sampleRate: -inf}"), expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserTest, BitwiseOperators) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::vector<std::string> bitwiseOperators{
        "$bitsAllClear", "$bitsAllSet", "$bitsAnyClear", "$bitsAnySet"};
    for (auto& bitwiseOperator : bitwiseOperators) {
        // Test accepting valid type coercion
        auto result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(1))), expCtx);
        ASSERT_TRUE(result.isOK());
        result = MatchExpressionParser::parse(BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(1LL))),
                                              expCtx);
        ASSERT_TRUE(result.isOK());
        result = MatchExpressionParser::parse(BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(1.0))),
                                              expCtx);
        ASSERT_TRUE(result.isOK());

        // Test rejecting overflow values.
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(1e30))), expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(-1e30))), expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);

        // Test rejecting non-integral values.
        result = MatchExpressionParser::parse(BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(1.5))),
                                              expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);

        // Test rejecting negative values.
        result = MatchExpressionParser::parse(BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(-1))),
                                              expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(-1LL))), expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(-1.0))), expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);

        // Test rejecting non-numeric values.
        result = MatchExpressionParser::parse(
            BSON("x" << BSON(bitwiseOperator << BSON_ARRAY("string"))), expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
        result = MatchExpressionParser::parse(BSON("x" << BSON(bitwiseOperator << BSON_ARRAY(NAN))),
                                              expCtx);
        ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
    }
}

TEST(InternalBinDataSubTypeMatchExpressionTest, InvalidSubTypeDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query1 = BSON("a" << BSON("$_internalSchemaBinDataSubType" << "foo"));
    auto query2 = BSON("a" << BSON("$_internalSchemaBinDataSubType" << "5"));
    auto statusWith1 = MatchExpressionParser::parse(query1, expCtx);
    auto statusWith2 = MatchExpressionParser::parse(query2, expCtx);
    ASSERT_NOT_OK(statusWith1.getStatus());
    ASSERT_NOT_OK(statusWith2.getStatus());
}

TEST(InternalBinDataSubTypeMatchExpressionTest, InvalidNumericalSubTypeDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query1 = BSON("a" << BSON("$_internalSchemaBinDataSubType" << 99));
    auto query2 = BSON("a" << BSON("$_internalSchemaBinDataSubType" << 2.1));
    auto statusWith1 = MatchExpressionParser::parse(query1, expCtx);
    auto statusWith2 = MatchExpressionParser::parse(query2, expCtx);
    ASSERT_NOT_OK(statusWith1.getStatus());
    ASSERT_NOT_OK(statusWith2.getStatus());
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, InvalidArgumentDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(
                  BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << "bar")), expCtx)
                  .getStatus(),
              ErrorCodes::BadValue);
    ASSERT_EQ(MatchExpressionParser::parse(
                  BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << "0")), expCtx)
                  .getStatus(),
              ErrorCodes::BadValue);
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, InvalidNumericalArgumentDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSON_ARRAY(0.21))), expCtx)
            .getStatus(),
        ErrorCodes::BadValue);
    ASSERT_EQ(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSON_ARRAY(13.3))), expCtx)
            .getStatus(),
        ErrorCodes::BadValue);
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, NonBsonTypeArgumentDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(
                  BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                   << BSON_ARRAY(stdx::to_underlying(BSONType::jsTypeMax) + 1))),
                  expCtx)
                  .getStatus(),
              ErrorCodes::BadValue);
}

}  // namespace mongo
