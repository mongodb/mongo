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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <set>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

TEST(MatchExpressionParserSchemaTest, UniqueItemsFailsToParseNonTrueArguments) {
    auto queryIntArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(queryIntArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << ""));
    expr = MatchExpressionParser::parse(queryStringArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << 1.0));
    expr = MatchExpressionParser::parse(queryDoubleArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaUniqueItems" << false));
    expr = MatchExpressionParser::parse(queryFalseArgument, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchOnlyAcceptsAnObjectArgument) {
    auto query = BSON("a" << BSON("$_internalSchemaObjectMatch" << 1));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    query = BSON("a" << BSON("$_internalSchemaObjectMatch" << "string"));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);

    query = BSON(
        "a" << BSON("$_internalSchemaObjectMatch" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(result.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchSubExprRejectsPathlessOperators) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    $expr: {$eq: ['$a', 5]}"
        "}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(result.getStatus(), ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, MinLengthFailsToParseNonIntegerArguments) {
    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaMinLength" << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(queryStringArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryEmptyStringArgument = BSON("x" << BSON("$_internalSchemaMinLength" << ""));
    expr = MatchExpressionParser::parse(queryEmptyStringArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaMinLength" << 1.5));
    expr = MatchExpressionParser::parse(queryDoubleArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaMinLength" << false));
    expr = MatchExpressionParser::parse(queryFalseArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryArrArgument = BSON("x" << BSON("$_internalSchemaMinLength" << BSON_ARRAY(1)));
    expr = MatchExpressionParser::parse(queryArrArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(MatchExpressionParserSchemaTest, MaxLengthFailsToParseNonIntegerArguments) {
    auto queryStringArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(queryStringArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryEmptyStringArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << ""));
    expr = MatchExpressionParser::parse(queryEmptyStringArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryDoubleArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << 1.5));
    expr = MatchExpressionParser::parse(queryDoubleArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryFalseArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << false));
    expr = MatchExpressionParser::parse(queryFalseArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());

    auto queryArrArgument = BSON("x" << BSON("$_internalSchemaMaxLength" << BSON_ARRAY(1)));
    expr = MatchExpressionParser::parse(queryArrArgument, expCtx);
    ASSERT_NOT_OK(expr.getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondFailsToParseNonObjectArguments) {
    auto queryWithInteger = fromjson("{$_internalSchemaCond: [1, {foo: 'bar'}, {baz: 7}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryWithInteger, expCtx).getStatus());


    auto queryWithArray = fromjson("{$_internalSchemaCond: [{foo: 'bar'}, [{qux: 3}], {baz: 7}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryWithArray, expCtx).getStatus());

    auto queryWithString = fromjson("{$_internalSchemaCond: [{foo: 'bar'}, {baz: 7}, 'blah']}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryWithString, expCtx).getStatus());
}

TEST(MatchExpressionParserSchemaTest, CondFailsToParseIfNotExactlyThreeArguments) {
    auto queryNoArguments = fromjson("{$_internalSchemaCond: []}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryNoArguments, expCtx).getStatus());

    auto queryOneArgument = fromjson("{$_internalSchemaCond: [{height: 171}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryOneArgument, expCtx).getStatus());

    auto queryFourArguments = fromjson(
        "{$_internalSchemaCond: [{make: 'lamborghini'}, {model: 'ghost'}, {color: 'celadon'}, "
        "{used: false}]}");
    ASSERT_EQ(ErrorCodes::FailedToParse,
              MatchExpressionParser::parse(queryFourArguments, expCtx).getStatus());
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseNonObjectArguments) {
    auto query = fromjson("{foo: {$_internalSchemaMatchArrayIndex: 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson("{foo: {$_internalSchemaMatchArrayIndex: []}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "[{index: 5, namePlaceholder: 'i', expression: {i: 1}}]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseIfPlaceholderNotValid) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 5, namePlaceholder: 7, expression: {i: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 5, namePlaceholder: 'Z', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseIfIndexNotANonnegativeInteger) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 'blah', namePlaceholder: 'i', expression: {i: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: -1, namePlaceholder: 'i', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 3.14, namePlaceholder: 'i', expression: {i: 1}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexFailsToParseIfExpressionNotValid) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: 'blah'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {doesntMatchThePlaceholder: 7}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {$invalid: 'blah'}}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithNegativeIndex) {
    BSONObj matchPredicate =
        fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [-2, {a: { $lt: 0 }}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithNonObjectExpression) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [-2, 4]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithInvalidExpression) {
    BSONObj matchPredicate =
        fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [-2, {$fakeExpression: 4}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FailsToParseWithEmptyArray) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: []}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    ASSERT_EQ(expr.getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, InternalTypeFailsToParseOnTypeMismatch) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType" << BSONObj()));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserSchemaTest, InternalTypeCanParseNumberAlias) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType" << "number"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT_EQ(result.getValue()->matchType(), MatchExpression::INTERNAL_SCHEMA_TYPE);
    auto typeExpr = static_cast<const InternalSchemaTypeExpression*>(result.getValue().get());
    ASSERT_TRUE(typeExpr->typeSet().allNumbers);
}

TEST(MatchExpressionParserSchemaTest, InternalTypeCanParseLongAlias) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType" << "long"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT_EQ(result.getValue()->matchType(), MatchExpression::INTERNAL_SCHEMA_TYPE);
    auto typeExpr = static_cast<const InternalSchemaTypeExpression*>(result.getValue().get());
    ASSERT_FALSE(typeExpr->typeSet().allNumbers);
    ASSERT_EQ(typeExpr->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(typeExpr->typeSet().hasType(BSONType::numberLong));
}

TEST(MatchExpressionParserSchemaTest, InternalTypeCanParseLongCode) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaType" << 18));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT_EQ(result.getValue()->matchType(), MatchExpression::INTERNAL_SCHEMA_TYPE);
    auto typeExpr = static_cast<const InternalSchemaTypeExpression*>(result.getValue().get());
    ASSERT_FALSE(typeExpr->typeSet().allNumbers);
    ASSERT_EQ(typeExpr->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(typeExpr->typeSet().hasType(BSONType::numberLong));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfAFieldIsMissing) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{namePlaceholder: 'i', patternProperties: [], otherwise: {i: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], patternProperties: [], otherwise: {i: 1}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', otherwise: {i: 1}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: []}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfNamePlaceholderNotAString) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 7, patternProperties: [], otherwise: {i: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: /i/, patternProperties: [], otherwise: {i: 1}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfNamePlaceholderNotValid) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'Capital',"
        "patternProperties: [], otherwise: {Capital: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: '', patternProperties: [], otherwise: {'': 1}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfPropertiesNotAllStrings) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [7], namePlaceholder: 'i', patternProperties: [], otherwise: {i: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['x', {}], namePlaceholder: 'i',"
        "patternProperties: [], otherwise: {i: 1}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesNotAllObjects) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: ['blah'], otherwise: {i: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 1}, patternProperties: [{regex: /a/, expression: {i: 0}}, 'blah']}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesHasUnknownFields) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [{foo: 1, bar: 1}], otherwise: {i: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, blah: 0}], otherwise: {i: 1}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesRegexMissingOrWrongType) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{expression: {i: 0}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: 7, expression: {i: 0}}]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: 'notARegex', expression: {i: 0}}]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesExpressionInvalid) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: /a/}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: /a/, expression: 'blah'}]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest,
     AllowedPropertiesFailsParsingIfPatternPropertiesRegexHasFlags) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "otherwise: {i: 0}, patternProperties: [{regex: /a/i, expression: {i: 0}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfMismatchingNamePlaceholders) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: {j: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);

    query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {w: 7}}], otherwise: {i: 'foo'}}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::FailedToParse);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfOtherwiseIncorrectType) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: false}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: [{i: 7}]}}");
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesFailsParsingIfOtherwiseNotAValidExpression) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [], otherwise: {i: {$invalid: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQ(MatchExpressionParser::parse(query, expCtx).getStatus(), ErrorCodes::BadValue);
}

TEST(MatchExpressionParserSchemaTest, RootDocEqFailsToParseNonObjects) {
    auto query = fromjson("{$_internalSchemaRootDocEq: 1}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rootDocEq = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(rootDocEq.getStatus(), ErrorCodes::TypeMismatch);

    query = fromjson("{$_internalSchemaRootDocEq: [{}]}");
    rootDocEq = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(rootDocEq.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(MatchExpressionParserSchemaTest, RootDocEqMustApplyToTopLevelDocument) {
    auto query = fromjson("{a: {$_internalSchemaRootDocEq: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rootDocEq = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(rootDocEq.getStatus(), ErrorCodes::BadValue);

    query = fromjson("{$or: [{a: 1}, {$_internalSchemaRootDocEq: {}}]}");
    rootDocEq = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(rootDocEq.getStatus());

    query = fromjson("{a: {$elemMatch: {$_internalSchemaRootDocEq: 1}}}");
    rootDocEq = MatchExpressionParser::parse(query, expCtx);
    ASSERT_EQ(rootDocEq.getStatus(), ErrorCodes::FailedToParse);
}

}  // namespace
}  // namespace mongo
