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

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

constexpr CollatorInterface* kSimpleCollator = nullptr;

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}


TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, UniqueItemsFailsToParseNonTrueArguments) {
    auto queryIntArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 0));
    auto expr = MatchExpressionParser::parse(
        queryIntArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaUniqueItems"
                                                << ""));
    expr = MatchExpressionParser::parse(
        queryStringArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 1.0));
    expr = MatchExpressionParser::parse(
        queryDoubleArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << false));
    expr = MatchExpressionParser::parse(
        queryFalseArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, UniqueItemsParsesTrueBooleanArgument) {
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

TEST(MatchExpressionParserSchemaTest, ObjectMatchOnlyAcceptsAnObjectArgument) {
    auto query = BSON("a" << BSON("$_internalSchemaObjectMatch" << 1));
    auto result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    query = BSON("a" << BSON("$_internalSchemaObjectMatch"
                             << "string"));
    result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    query = BSON(
        "a" << BSON("$_internalSchemaObjectMatch" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1))));
    result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchCorrectlyParsesObjects) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    b: {$gte: 0}"
        "    }}"
        "}");
    auto result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: 'string'}}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: -1}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: {b: 1}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: [{b: 0}]}")));
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchCorrectlyParsesNestedObjectMatch) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    b: {$_internalSchemaObjectMatch: {"
        "        $or: [{c: {$type: 'string'}}, {c: {$gt: 0}}]"
        "    }}"
        "}}}");
    auto result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: {}}}}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: 0}}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: 'string'}}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: 1}}}")));
    ASSERT_TRUE(
        result.getValue()->matchesBSON(fromjson("{a: [{b: 0}, {b: [{c: 0}, {c: 'string'}]}]}")));
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchSubExprRejectsTopLevelOperators) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    $isolated: 1"
        "}}}");
    auto result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

//
// Tests for parsing the $_internalSchemaMinLength expression.
//
TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthFailsToParseNonIntegerArguments) {
    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaMinLength"
                                                << "abc"));
    auto expr = MatchExpressionParser::parse(
        queryStringArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryEmptyStringArgument = BSON("x" << BSON("$_internalSchemaMinLength"
                                                     << ""));
    expr = MatchExpressionParser::parse(
        queryEmptyStringArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaMinLength" << 1.5));
    expr = MatchExpressionParser::parse(
        queryDoubleArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaMinLength" << false));
    expr = MatchExpressionParser::parse(
        queryFalseArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryArrArgument = BSON("x" << BSON("$_internalSchemaMinLength" << BSON_ARRAY(1)));
    expr = MatchExpressionParser::parse(
        queryArrArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

//
// Tests for parsing the $_internalSchemaMaxLength expression.
//
TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "a")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ab")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthFailsToParseNonIntegerArguments) {
    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaMaxLength"
                                                << "abc"));
    auto expr = MatchExpressionParser::parse(
        queryStringArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryEmptyStringArgument = BSON("x" << BSON("$_internalSchemaMaxLength"
                                                     << ""));
    expr = MatchExpressionParser::parse(
        queryEmptyStringArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << 1.5));
    expr = MatchExpressionParser::parse(
        queryDoubleArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << false));
    expr = MatchExpressionParser::parse(
        queryFalseArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryArrArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << BSON_ARRAY(1)));
    expr = MatchExpressionParser::parse(
        queryArrArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondFailsToParseNonObjectArguments) {
    auto queryWithInteger = fromjson("{$_internalSchemaCond: [1, {foo: 'bar'}, {baz: 7}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(
                  queryWithInteger, ExtensionsCallbackDisallowExtensions(), kSimpleCollator)
                  .getStatus());


    auto queryWithArray = fromjson("{$_internalSchemaCond: [{foo: 'bar'}, [{qux: 3}], {baz: 7}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(
                  queryWithArray, ExtensionsCallbackDisallowExtensions(), kSimpleCollator)
                  .getStatus());

    auto queryWithString = fromjson("{$_internalSchemaCond: [{foo: 'bar'}, {baz: 7}, 'blah']}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(
                  queryWithString, ExtensionsCallbackDisallowExtensions(), kSimpleCollator)
                  .getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondFailsToParseIfNotExactlyThreeArguments) {
    auto queryNoArguments = fromjson("{$_internalSchemaCond: []}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(
                  queryNoArguments, ExtensionsCallbackDisallowExtensions(), kSimpleCollator)
                  .getStatus());

    auto queryOneArgument = fromjson("{$_internalSchemaCond: [{height: 171}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(
                  queryOneArgument, ExtensionsCallbackDisallowExtensions(), kSimpleCollator)
                  .getStatus());

    auto queryFourArguments = fromjson(
        "{$_internalSchemaCond: [{make: 'lamborghini'}, {model: 'ghost'}, {color: 'celadon'}, "
        "{used: false}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(
                  queryFourArguments, ExtensionsCallbackDisallowExtensions(), kSimpleCollator)
                  .getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondParsesThreeMatchExpresssions) {
    auto query = fromjson(
        "{$_internalSchemaCond: [{climate: 'rainy'}, {clothing: 'jacket'}, {clothing: 'shirt'}]}");
    auto expr = MatchExpressionParser::parse(
        query, ExtensionsCallbackDisallowExtensions(), kSimpleCollator);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(expr.getValue()->matchesBSON(
        fromjson("{climate: 'rainy', clothing: ['jacket', 'umbrella']}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(
        fromjson("{climate: 'sunny', clothing: ['shirt', 'shorts']}")));
    ASSERT_FALSE(
        expr.getValue()->matchesBSON(fromjson("{climate: 'rainy', clothing: ['poncho']}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{clothing: ['jacket']}")));
}
}  // namespace
}  // namespace mongo
