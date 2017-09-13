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
#include "mongo/db/matcher/expression_type.h"
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2LL));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}


TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2.0));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << Decimal128("2")));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, UniqueItemsFailsToParseNonTrueArguments) {
    auto queryIntArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 0));
    auto expr = MatchExpressionParser::parse(queryIntArgument, kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaUniqueItems"
                                                << ""));
    expr = MatchExpressionParser::parse(queryStringArgument, kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 1.0));
    expr = MatchExpressionParser::parse(queryDoubleArgument, kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << false));
    expr = MatchExpressionParser::parse(queryFalseArgument, kSimpleCollator);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, UniqueItemsParsesTrueBooleanArgument) {
    auto query = BSON("x" << BSON("$_internalSchemaUniqueItems" << true));
    auto expr = MatchExpressionParser::parse(query, kSimpleCollator);
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
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    query = BSON("a" << BSON("$_internalSchemaObjectMatch"
                             << "string"));
    result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    query = BSON(
        "a" << BSON("$_internalSchemaObjectMatch" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1))));
    result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchCorrectlyParsesObjects) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    b: {$gte: 0}"
        "    }}"
        "}");
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: 'string'}}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: -1}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: [{b: 0}]}")));
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchCorrectlyParsesNestedObjectMatch) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    b: {$_internalSchemaObjectMatch: {"
        "        $or: [{c: {$type: 'string'}}, {c: {$gt: 0}}]"
        "    }}"
        "}}}");
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: {}}}}")));
    ASSERT_FALSE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: 0}}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: 'string'}}}")));
    ASSERT_TRUE(result.getValue()->matchesBSON(fromjson("{a: {b: {c: 1}}}")));
    ASSERT_FALSE(
        result.getValue()->matchesBSON(fromjson("{a: [{b: 0}, {b: [{c: 0}, {c: 'string'}]}]}")));
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchSubExprRejectsTopLevelOperators) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    $isolated: 1"
        "}}}");
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

//
// Tests for parsing the $_internalSchemaMinLength expression.
//
TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    auto expr = MatchExpressionParser::parse(queryStringArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryEmptyStringArgument = BSON("x" << BSON("$_internalSchemaMinLength"
                                                     << ""));
    expr = MatchExpressionParser::parse(queryEmptyStringArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaMinLength" << 1.5));
    expr = MatchExpressionParser::parse(queryDoubleArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaMinLength" << false));
    expr = MatchExpressionParser::parse(queryFalseArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryArrArgument = BSON("x" << BSON("$_internalSchemaMinLength" << BSON_ARRAY(1)));
    expr = MatchExpressionParser::parse(queryArrArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

//
// Tests for parsing the $_internalSchemaMaxLength expression.
//
TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, kSimpleCollator);
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
    auto expr = MatchExpressionParser::parse(queryStringArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryEmptyStringArgument = BSON("x" << BSON("$_internalSchemaMaxLength"
                                                     << ""));
    expr = MatchExpressionParser::parse(queryEmptyStringArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << 1.5));
    expr = MatchExpressionParser::parse(queryDoubleArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << false));
    expr = MatchExpressionParser::parse(queryFalseArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryArrArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << BSON_ARRAY(1)));
    expr = MatchExpressionParser::parse(queryArrArgument, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondFailsToParseNonObjectArguments) {
    auto queryWithInteger = fromjson("{$_internalSchemaCond: [1, {foo: 'bar'}, {baz: 7}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryWithInteger, kSimpleCollator).getStatus());


    auto queryWithArray = fromjson("{$_internalSchemaCond: [{foo: 'bar'}, [{qux: 3}], {baz: 7}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryWithArray, kSimpleCollator).getStatus());

    auto queryWithString = fromjson("{$_internalSchemaCond: [{foo: 'bar'}, {baz: 7}, 'blah']}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryWithString, kSimpleCollator).getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondFailsToParseIfNotExactlyThreeArguments) {
    auto queryNoArguments = fromjson("{$_internalSchemaCond: []}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryNoArguments, kSimpleCollator).getStatus());

    auto queryOneArgument = fromjson("{$_internalSchemaCond: [{height: 171}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryOneArgument, kSimpleCollator).getStatus());

    auto queryFourArguments = fromjson(
        "{$_internalSchemaCond: [{make: 'lamborghini'}, {model: 'ghost'}, {color: 'celadon'}, "
        "{used: false}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryFourArguments, kSimpleCollator).getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondParsesThreeMatchExpresssions) {
    auto query = fromjson(
        "{$_internalSchemaCond: [{climate: 'rainy'}, {clothing: 'jacket'}, {clothing: 'shirt'}]}");
    auto expr = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(expr.getValue()->matchesBSON(
        fromjson("{climate: 'rainy', clothing: ['jacket', 'umbrella']}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(
        fromjson("{climate: 'sunny', clothing: ['shirt', 'shorts']}")));
    ASSERT_FALSE(
        expr.getValue()->matchesBSON(fromjson("{climate: 'rainy', clothing: ['poncho']}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{clothing: ['jacket']}")));
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseNonObjectArguments) {
    auto query = fromjson("{foo: {$_internalSchemaMatchArrayIndex: 7}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson("{foo: {$_internalSchemaMatchArrayIndex: []}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "[{index: 5, namePlaceholder: 'i', expression: {i: 1}}]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseIfPlaceholderNotValid) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 5, namePlaceholder: 7, expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 5, namePlaceholder: 'Z', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseIfIndexNotANonnegativeInteger) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 'blah', namePlaceholder: 'i', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: -1, namePlaceholder: 'i', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 3.14, namePlaceholder: 'i', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseIfExpressionNotValid) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: 'blah'}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {doesntMatchThePlaceholder: 7}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {$invalid: 'blah'}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexParsesSuccessfully) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}");
    auto matchArrayIndex = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(matchArrayIndex.getStatus());

    ASSERT_TRUE(matchArrayIndex.getValue()->matchesBSON(fromjson("{foo: [-1, 0, 1]}")));
    ASSERT_FALSE(matchArrayIndex.getValue()->matchesBSON(fromjson("{foo: [2, 'blah']}")));
    ASSERT_FALSE(matchArrayIndex.getValue()->matchesBSON(fromjson("{foo: [{x: 'baz'}]}")));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithNegativeIndex) {
    BSONObj matchPredicate =
        fromjson("{$_internalSchemaAllElemMatchFromIndex: [-2, {a: { $lt: 0 }}]}");
    auto expr = MatchExpressionParser::parse(matchPredicate, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithNonObjectExpression) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaAllElemMatchFromIndex: [-2, 4]}");
    auto expr = MatchExpressionParser::parse(matchPredicate, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithInvalidExpression) {
    BSONObj matchPredicate =
        fromjson("{$_internalSchemaAllElemMatchFromIndex: [-2, {$fakeExpression: 4}]}");
    auto expr = MatchExpressionParser::parse(matchPredicate, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithEmptyArray) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaAllElemMatchFromIndex: []}");
    auto expr = MatchExpressionParser::parse(matchPredicate, kSimpleCollator);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, ParsesCorreclyWithValidInput) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: { $lt: 4 }}]}}");
    auto expr = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{a: [5, 3, 3, 3, 3, 3]}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{a: [3, 3, 3, 5, 3, 3]}")));
}

TEST(MatchExpressionParserSchemaTest, InternalTypeFailsToParseOnTypeMismatch) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType" << BSONObj()));
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserSchemaTest, InternalTypeCanParseNumberAlias) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType"
                                     << "number"));
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT_EQ(result.getValue()->matchType(), MatchExpression::INTERNAL_SCHEMA_TYPE);
    auto typeExpr = static_cast<const InternalSchemaTypeExpression*>(result.getValue().get());
    ASSERT_TRUE(typeExpr->typeSet().allNumbers);
}

TEST(MatchExpressionParserSchemaTest, InternalTypeCanParseLongAlias) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType"
                                     << "long"));
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT_EQ(result.getValue()->matchType(), MatchExpression::INTERNAL_SCHEMA_TYPE);
    auto typeExpr = static_cast<const InternalSchemaTypeExpression*>(result.getValue().get());
    ASSERT_FALSE(typeExpr->typeSet().allNumbers);
    ASSERT_EQ(typeExpr->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(typeExpr->typeSet().hasType(BSONType::NumberLong));
}

TEST(MatchExpressionParserSchemaTest, InternalTypeCanParseLongCode) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType" << 18));
    auto result = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(result.getStatus());

    ASSERT_EQ(result.getValue()->matchType(), MatchExpression::INTERNAL_SCHEMA_TYPE);
    auto typeExpr = static_cast<const InternalSchemaTypeExpression*>(result.getValue().get());
    ASSERT_FALSE(typeExpr->typeSet().allNumbers);
    ASSERT_EQ(typeExpr->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(typeExpr->typeSet().hasType(BSONType::NumberLong));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfAFieldIsMissing) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{namePlaceholder: 'i', patternProperties: [], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], patternProperties: [], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: []}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfNamePlaceholderNotAString) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 7, patternProperties: [], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: /i/, patternProperties: [], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfNamePlaceholderNotValid) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'Capital',"
        "patternProperties: [], otherwise: {Capital: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::BadValue);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: '', patternProperties: [], otherwise: {'': 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfPropertiesNotAllStrings) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [7], namePlaceholder: 'i', patternProperties: [], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['x', {}], namePlaceholder: 'i',"
        "patternProperties: [], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesNotAllObjects) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: ['blah'], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 1}, patternProperties: [{regex: /a/, expression: {i: 0}}, 'blah']}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesHasUnknownFields) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [{foo: 1, bar: 1}], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, blah: 0}], otherwise: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesRegexMissingOrWrongType) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{expression: {i: 0}}]}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: 7, expression: {i: 0}}]}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: 'notARegex', expression: {i: 0}}]}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesExpressionInvalid) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: /a/}]}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: /a/, expression: 'blah'}]}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesRegexHasFlags) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: /a/i, expression: {i: 0}}]}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfMismatchingNamePlaceholders) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: {j: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {w: 7}}], otherwise: {i: 'foo'}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfOtherwiseIncorrectType) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: false}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: [{i: 7}]}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfOtherwiseNotAValidExpression) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [], otherwise: {i: {$invalid: 1}}}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, kSimpleCollator).getStatus(),
              ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesParsesSuccessfully) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['phoneNumber', 'address'],"
        "namePlaceholder: 'i', otherwise: {i: {$gt: 10}},"
        "patternProperties: [{regex: /[nN]umber/, expression: {i: {$type: 'number'}}}]}}}");
    auto allowedProperties = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(allowedProperties.getStatus());

    ASSERT_TRUE(allowedProperties.getValue()->matchesBSON(
        fromjson("{phoneNumber: 123, address: 'earth'}")));
    ASSERT_TRUE(allowedProperties.getValue()->matchesBSON(
        fromjson("{phoneNumber: 3.14, workNumber: 456}")));

    ASSERT_FALSE(allowedProperties.getValue()->matchesBSON(fromjson("{otherNumber: 'blah'}")));
    ASSERT_FALSE(allowedProperties.getValue()->matchesBSON(fromjson("{phoneNumber: 'blah'}")));
    ASSERT_FALSE(allowedProperties.getValue()->matchesBSON(fromjson("{other: 'blah'}")));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesAcceptsEmptyPropertiesAndPatternProperties) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: {i: 1}}}}");
    auto allowedProperties = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(allowedProperties.getStatus());

    ASSERT_TRUE(allowedProperties.getValue()->matchesBSON(BSONObj()));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesAcceptsEmptyOtherwiseExpression) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: {}}}}}");
    auto allowedProperties = MatchExpressionParser::parse(query, kSimpleCollator);
    ASSERT_OK(allowedProperties.getStatus());

    ASSERT_TRUE(allowedProperties.getValue()->matchesBSON(BSONObj()));
}

}  // namespace
}  // namespace mongo
