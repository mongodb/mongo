// expression_parser_test.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

TEST(AtomicMatchExpressionTest, AtomicOperatorSuccessfullyParsesWhenFeatureBitIsSet) {
    auto query = BSON("x" << 5 << "$atomic" << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kIsolated);
    ASSERT_OK(result.getStatus());
}

TEST(AtomicMatchExpressionTest, AtomicOperatorFailsToParseIfNotTopLevel) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("x" << 5 << "y" << BSON("$atomic" << 1));
    auto result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kIsolated);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::BadValue, result.getStatus());
}

TEST(AtomicMatchExpressionTest, AtomicOperatorFailsToParseIfFeatureBitIsNotSet) {
    auto query = BSON("x" << 5 << "$atomic" << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(ErrorCodes::QueryFeatureNotAllowed, result.getStatus());
}

TEST(IsolatedMatchExpressionTest, IsolatedOperatorSuccessfullyParsesWhenFeatureBitIsSet) {
    auto query = BSON("x" << 5 << "$isolated" << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kIsolated);
    ASSERT_OK(result.getStatus());
}

TEST(IsolatedMatchExpressionTest, IsolatedOperatorFailsToParseIfFeatureBitIsNotSet) {
    // Query parsing fails if $isolated is not in the allowed feature set.
    auto query = BSON("x" << 5 << "$isolated" << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::QueryFeatureNotAllowed, result.getStatus());
}

TEST(IsolatedMatchExpressionTest, IsolatedOperatorFailsToParseIfNotTopLevel) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("x" << 5 << "y" << BSON("$isolated" << 1));
    auto result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::AllowedFeatures::kIsolated);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::BadValue, result.getStatus());
}

TEST(MatchExpressionParserTest, MinDistanceWithoutNearFailsToParse) {
    BSONObj query = fromjson("{loc: {$minDistance: 10}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongRejectsNegative) {
    BSONObj query = BSON("" << -2LL);
    ASSERT_NOT_OK(
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongAcceptsNegative) {
    BSONObj query = BSON("" << -2LL);
    auto result = MatchExpressionParser::parseIntegerElementToLong(query.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(-2LL, result.getValue());
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongRejectsTooLargeDouble) {
    BSONObj query = BSON("" << MatchExpressionParser::kLongLongMaxPlusOneAsDouble);
    ASSERT_NOT_OK(
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongRejectsTooLargeDouble) {
    BSONObj query = BSON("" << MatchExpressionParser::kLongLongMaxPlusOneAsDouble);
    ASSERT_NOT_OK(MatchExpressionParser::parseIntegerElementToLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongRejectsTooLargeNegativeDouble) {
    BSONObj query = BSON("" << std::numeric_limits<double>::min());
    ASSERT_NOT_OK(MatchExpressionParser::parseIntegerElementToLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongRejectsString) {
    BSONObj query = BSON(""
                         << "1");
    ASSERT_NOT_OK(
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongRejectsString) {
    BSONObj query = BSON(""
                         << "1");
    ASSERT_NOT_OK(MatchExpressionParser::parseIntegerElementToLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongRejectsNonIntegralDouble) {
    BSONObj query = BSON("" << 2.5);
    ASSERT_NOT_OK(
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongRejectsNonIntegralDouble) {
    BSONObj query = BSON("" << 2.5);
    ASSERT_NOT_OK(MatchExpressionParser::parseIntegerElementToLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongRejectsNonIntegralDecimal) {
    BSONObj query = BSON("" << Decimal128("2.5"));
    ASSERT_NOT_OK(
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongRejectsNonIntegralDecimal) {
    BSONObj query = BSON("" << Decimal128("2.5"));
    ASSERT_NOT_OK(MatchExpressionParser::parseIntegerElementToLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongRejectsLargestDecimal) {
    BSONObj query = BSON("" << Decimal128(Decimal128::kLargestPositive));
    ASSERT_NOT_OK(
        MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongRejectsLargestDecimal) {
    BSONObj query = BSON("" << Decimal128(Decimal128::kLargestPositive));
    ASSERT_NOT_OK(MatchExpressionParser::parseIntegerElementToLong(query.firstElement()));
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongAcceptsZero) {
    BSONObj query = BSON("" << 0);
    auto result = MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 0LL);
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongAcceptsZero) {
    BSONObj query = BSON("" << 0);
    auto result = MatchExpressionParser::parseIntegerElementToLong(query.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 0LL);
}

TEST(MatchExpressionParserTest, ParseIntegerElementToNonNegativeLongAcceptsThree) {
    BSONObj query = BSON("" << 3.0);
    auto result = MatchExpressionParser::parseIntegerElementToNonNegativeLong(query.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 3LL);
}

TEST(MatchExpressionParserTest, ParseIntegerElementToLongAcceptsThree) {
    BSONObj query = BSON("" << 3.0);
    auto result = MatchExpressionParser::parseIntegerElementToLong(query.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), 3LL);
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
}  // namespace mongo
