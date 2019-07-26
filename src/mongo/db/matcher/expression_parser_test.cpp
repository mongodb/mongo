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

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"

namespace mongo {

TEST(MatchExpressionParserTest, SimpleEQ1) {
    BSONObj query = BSON("x" << 2);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserTest, Multiple1) {
    BSONObj query = BSON("x" << 5 << "y" << BSON("$gt" << 5 << "$lt" << 8));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 7)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 6)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 6 << "y" << 7)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 9)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 4)));
}

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

TEST(MatchExpressionParserTest, AlwaysFalseParsesIntegerArgument) {
    auto query = BSON(AlwaysFalseMatchExpression::kName << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: 1}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: 'blah'}")));
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

TEST(MatchExpressionParserTest, AlwaysTrueParsesIntegerArgument) {
    auto query = BSON(AlwaysTrueMatchExpression::kName << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: 1}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: 'blah'}")));
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
    auto query = BSON("a" << BSON("$options"
                                  << "s"
                                  << "$regex" << BSONRegEx("/myRegex/", "i")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithOptionsFirst) {
    auto query = BSON("a" << BSON("$options"
                                  << "s"
                                  << "$regex" << BSONRegEx("/myRegex/", "")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(MatchExpressionParser::parse(query, expCtx).getStatus());
}

TEST(MatchExpressionParserTest, RegexParsesSuccessfullyWithOptionsFirstEmptyOptions) {
    auto query = BSON("a" << BSON("$options"
                                  << ""
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

TEST(MatchExpressionParserTest, InternalExprEqParsesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto query = fromjson("{a: {$_internalExprEq: 'foo'}}");
    auto statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());
    ASSERT_TRUE(statusWith.getValue()->matchesBSON(fromjson("{a: 'foo'}")));
    ASSERT_TRUE(statusWith.getValue()->matchesBSON(fromjson("{a: ['foo']}")));
    ASSERT_TRUE(statusWith.getValue()->matchesBSON(fromjson("{a: ['bar']}")));
    ASSERT_FALSE(statusWith.getValue()->matchesBSON(fromjson("{a: 'bar'}")));

    query = fromjson("{'a.b': {$_internalExprEq: 5}}");
    statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());
    ASSERT_TRUE(statusWith.getValue()->matchesBSON(fromjson("{a: {b: 5}}")));
    ASSERT_TRUE(statusWith.getValue()->matchesBSON(fromjson("{a: {b: [5]}}")));
    ASSERT_TRUE(statusWith.getValue()->matchesBSON(fromjson("{a: {b: [6]}}")));
    ASSERT_FALSE(statusWith.getValue()->matchesBSON(fromjson("{a: {b: 6}}")));
}

TEST(MatchesExpressionParserTest, InternalExprEqComparisonToArrayDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = fromjson("{'a.b': {$_internalExprEq: [5]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(MatchesExpressionParserTest, InternalExprEqComparisonToUndefinedDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = fromjson("{'a.b': {$_internalExprEq: undefined}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(InternalBinDataSubTypeMatchExpressionTest, SubTypeParsesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataSubType" << BinDataType::bdtCustom));
    auto statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());

    uint8_t bytes[] = {0, 1, 2, 3, 4, 5};
    BSONObj match = BSON("a" << BSONBinData(bytes, 5, BinDataType::bdtCustom));
    BSONObj notMatch = BSON("a" << BSONBinData(bytes, 5, BinDataType::Function));

    ASSERT_TRUE(statusWith.getValue()->matchesBSON(match));
    ASSERT_FALSE(statusWith.getValue()->matchesBSON(notMatch));
}

TEST(InternalBinDataSubTypeMatchExpressionTest, SubTypeWithFloatParsesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataSubType" << 5.0));
    auto statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());

    uint8_t bytes[] = {0, 1, 2, 3, 4, 5};
    BSONObj match = BSON("a" << BSONBinData(bytes, 5, BinDataType::MD5Type));
    BSONObj notMatch = BSON("a" << BSONBinData(bytes, 5, BinDataType::bdtCustom));

    ASSERT_TRUE(statusWith.getValue()->matchesBSON(match));
    ASSERT_FALSE(statusWith.getValue()->matchesBSON(notMatch));
}

TEST(InternalBinDataSubTypeMatchExpressionTest, InvalidSubTypeDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query1 = BSON("a" << BSON("$_internalSchemaBinDataSubType"
                                   << "foo"));
    auto query2 = BSON("a" << BSON("$_internalSchemaBinDataSubType"
                                   << "5"));
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

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeMatchesSingleTypeAlias) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                  << "string"));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(expr->matchesBSON(matchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeMatchesSingleType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(expr->matchesBSON(matchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeMatchesOneOfTypesInArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                  << BSON_ARRAY(BSONType::Date << BSONType::String)));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(expr->matchesBSON(matchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeDoesNotMatchSingleType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::NumberInt;

    BSONObj notMatchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                     sizeof(FleBlobHeader),
                                                     BinDataType::Encrypt));
    ASSERT_FALSE(expr->matchesBSON(notMatchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeDoesNotMatchTypeArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                  << BSON_ARRAY(BSONType::Date << BSONType::Bool)));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::NumberInt;

    BSONObj notMatchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                     sizeof(FleBlobHeader),
                                                     BinDataType::Encrypt));
    ASSERT_FALSE(expr->matchesBSON(notMatchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, InvalidArgumentDoesNotParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                                            << "bar")),
                                           expCtx)
                  .getStatus(),
              ErrorCodes::BadValue);
    ASSERT_EQ(MatchExpressionParser::parse(BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                                            << "0")),
                                           expCtx)
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
    ASSERT_EQ(
        MatchExpressionParser::parse(BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                                      << BSON_ARRAY(BSONType::JSTypeMax + 1))),
                                     expCtx)
            .getStatus(),
        ErrorCodes::BadValue);
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, IntentToEncryptFleBlobDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::IntentToEncrypt;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;
    BSONObj notMatch = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                               sizeof(FleBlobHeader),
                                               BinDataType::Encrypt));

    ASSERT_FALSE(expr->matchesBSON(notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, UnknownFleBlobTypeDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = 6;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;
    BSONObj notMatch = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                               sizeof(FleBlobHeader),
                                               BinDataType::Encrypt));
    try {
        expr->matchesBSON(notMatch);
    } catch (...) {
        ASSERT_EQ(exceptionToStatus().code(), 33118);
    }
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, EmptyFleBlobDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Encrypt));
    ASSERT_FALSE(expr->matchesBSON(notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, NonEncryptBinDataSubTypeDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    BSONObj notMatch = BSON("a" << BSONBinData("\x69\xb7", 2, BinDataGeneral));
    ASSERT_FALSE(expr->matchesBSON(notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, NonBinDataValueDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::String));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    BSONObj notMatch = BSON("a" << BSONArray());
    ASSERT_FALSE(expr->matchesBSON(notMatch));
}
}  // namespace mongo
