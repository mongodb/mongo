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
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"

namespace mongo {

TEST(MatchExpressionParserTest, SimpleEQ1) {
    BSONObj query = BSON("x" << 2);
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserTest, Multiple1) {
    BSONObj query = BSON("x" << 5 << "y" << BSON("$gt" << 5 << "$lt" << 8));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 7)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 6)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 6 << "y" << 7)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 9)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5 << "y" << 4)));
}

TEST(AtomicMatchExpressionTest, AtomicOperator1) {
    BSONObj query = BSON("x" << 5 << "$atomic" << BSON("$gt" << 5 << "$lt" << 8));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    query = BSON("x" << 5 << "$isolated" << 1);
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    query = BSON("x" << 5 << "y" << BSON("$isolated" << 1));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserTest, MinDistanceWithoutNearFailsToParse) {
    BSONObj query = fromjson("{loc: {$minDistance: 10}}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
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

TEST(MatchExpressionParserTest, ParseTypeFromAliasCanParseNumberAlias) {
    auto result = MatchExpressionParser::parseTypeFromAlias("a", "number");
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue()->path(), "a");
    ASSERT_TRUE(result.getValue()->getType().allNumbers);
    ASSERT_TRUE(result.getValue()->matchesAllNumbers());
}

TEST(MatchExpressionParserTest, ParseTypeFromAliasCanParseLongAlias) {
    auto result = MatchExpressionParser::parseTypeFromAlias("a", "long");
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue()->path(), "a");
    ASSERT_FALSE(result.getValue()->getType().allNumbers);
    ASSERT_FALSE(result.getValue()->matchesAllNumbers());
    ASSERT_EQ(result.getValue()->getType().bsonType, BSONType::NumberLong);
    ASSERT_EQ(result.getValue()->getBSONType(), BSONType::NumberLong);
}

TEST(MatchExpressionParserTest, ParseTypeFromAliasFailsToParseUnknownAlias) {
    auto result = MatchExpressionParser::parseTypeFromAlias("a", "unknown");
    ASSERT_NOT_OK(result.getStatus());
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
    auto queryIntArgument = BSON("$alwaysFalse" << 0);
    auto expr = MatchExpressionParser::parse(
        queryIntArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON("$alwaysFalse"
                                    << "");
    expr = MatchExpressionParser::parse(
        queryStringArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON("$alwaysFalse" << 1.1);
    expr = MatchExpressionParser::parse(
        queryDoubleArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON("$alwaysFalse" << true);
    expr = MatchExpressionParser::parse(
        queryFalseArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserTest, AlwaysFalseParsesIntegerArgument) {
    auto query = BSON("$alwaysFalse" << 1);
    auto expr =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_OK(expr.getStatus());

    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: 1}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: 'blah'}")));
}

TEST(MatchExpressionParserTest, AlwaysTrueFailsToParseNonOneArguments) {
    auto queryIntArgument = BSON("$alwaysTrue" << 0);
    auto expr = MatchExpressionParser::parse(
        queryIntArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON("$alwaysTrue"
                                    << "");
    expr = MatchExpressionParser::parse(
        queryStringArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON("$alwaysTrue" << 1.1);
    expr = MatchExpressionParser::parse(
        queryDoubleArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON("$alwaysTrue" << true);
    expr = MatchExpressionParser::parse(
        queryFalseArgument, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserTest, AlwaysTrueParsesIntegerArgument) {
    auto query = BSON("$alwaysTrue" << 1);
    auto expr =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), nullptr);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: 1}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: 'blah'}")));
}
}
