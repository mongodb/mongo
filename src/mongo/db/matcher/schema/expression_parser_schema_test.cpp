/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

constexpr CollatorInterface* kSimpleCollator = nullptr;

//
// Tests for parsing the $_internalSchemaMinItems expression.
//
TEST(MatchExpressionParserInternalSchemaMinItemsTest, CorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserInternalSchemaMinItemsTest, CorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}


TEST(MatchExpressionParserInternalSchemaMinItemsTest, CorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserInternalSchemaMinItemsTest, CorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

//
// Tests for parsing the $_internalSchemaMaxItems expression.
//
TEST(MatchExpressionParserInternalSchemaMaxItemsTest, CorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserInternalSchemaMaxItemsTest, CorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}


TEST(MatchExpressionParserInternalSchemaMaxItemsTest, CorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserInternalSchemaMaxItemsTest, CorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserInternalSchemaUniqueItemsTest, FailsToParseNonTrueArguments) {
    auto queryIntArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 0));
    auto expr = MatchExpressionParser::parse(
        queryIntArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaUniqueItems"
                                                << ""));
    expr = MatchExpressionParser::parse(
        queryStringArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 1.0));
    expr = MatchExpressionParser::parse(
        queryDoubleArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << false));
    expr = MatchExpressionParser::parse(
        queryFalseArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(MatchExpressionParserInternalSchemaUniqueItemsTest, ParsesTrueBooleanArgument) {
    auto query = BSON("x" << BSON("$_internalSchemaUniqueItems" << true));
    auto expr = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(expr.getStatus());

    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: 1}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: 'blah'}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: []}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: [0]}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{x: ['7', null, [], {}, 7]}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: ['dup', 'dup', 7]}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{x: [{x: 1}, {x: 1}]}")));
}
}  // namespace
}  // namespace mongo
