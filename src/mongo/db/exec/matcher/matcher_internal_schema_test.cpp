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

#include "mongo/bson/json.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo::evaluate_internal_schema_matcher_test {

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchesEmptyQuery) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchesValidQueries) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4))));

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4))));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           BSON("a" << BSON_ARRAY(10 << 2 << 3 << 4))));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           BSON("a" << BSON_ARRAY(10 << 20 << 3 << 4))));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(),
                                            BSON("a" << BSON_ARRAY(1 << 2 << 3 << 40))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, RejectsNonArrayElements) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << BSON("a" << 1))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchesArraysWithLessElementsThanIndex) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, NestedArraysMatchSubexpression) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(1 << 2 << BSON_ARRAY(3 << 4) << 4))));
    ASSERT_TRUE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(1 << 2 << BSON_ARRAY(6 << 4) << 4))));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(1 << 2 << BSON_ARRAY(5 << 6) << 4))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchedQueriesWithDottedPaths) {
    auto query = fromjson("{'a.b': {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3 << 4)))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, FindsFirstMismatchInArray) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    auto elemMatchExpr = dynamic_cast<const InternalSchemaAllElemMatchFromIndexMatchExpression*>(
        expr.getValue().get());
    ASSERT(elemMatchExpr);
    ASSERT_FALSE(exec::matcher::findFirstMismatchInArray(
        elemMatchExpr, BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4)), nullptr));
    auto inputArray = BSON_ARRAY(1 << 2 << 3 << 3 << 6 << 7);
    auto mismatchedElement =
        exec::matcher::findFirstMismatchInArray(elemMatchExpr, inputArray, nullptr);
    ASSERT_TRUE(mismatchedElement);
    ASSERT_EQ(mismatchedElement.Int(), 6);
}

TEST(InternalSchemaAllowedPropertiesMatchExpression, MatchesObjectsWithListedProperties) {
    auto filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a', 'b'],"
        "namePlaceholder: 'i', patternProperties: [], otherwise: {i: 0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: 1, b: 1}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: 1}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{b: 1}")));
}

TEST(InternalSchemaAllowedPropertiesMatchExpression, MatchesObjectsWithMatchingPatternProperties) {
    auto filter = fromjson(R"(
        {$_internalSchemaAllowedProperties: {
            properties: [],
            namePlaceholder: 'i',
            patternProperties: [
                {regex: /s$/, expression: {i: {$gt: 0}}},
                {regex: /[nN]um/, expression: {i: {$type: 'number'}}}
            ],
            otherwise: {i: {$type: 'string'}}
        }})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           fromjson("{puppies: 2, kittens: 3, phoneNum: 1234}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{puppies: 2}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{phoneNum: 1234}")));
}

TEST(InternalSchemaAllowedPropertiesMatchExpression,
     PatternPropertiesStillEnforcedEvenIfFieldListedInProperties) {
    auto filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'a',"
        "patternProperties: [{regex: /a/, expression: {a: {$gt: 5}}}], otherwise: {a: 0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: 6}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: 5}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: 4}")));
}

TEST(InternalSchemaAllowedPropertiesMatchExpression, OtherwiseEnforcedWhenAppropriate) {
    auto filter = fromjson(R"(
        {$_internalSchemaAllowedProperties: {
            properties: [],
            namePlaceholder: 'i',
            patternProperties: [
                {regex: /s$/, expression: {i: {$gt: 0}}},
                {regex: /[nN]um/, expression: {i: {$type: 'number'}}}
            ],
            otherwise: {i: {$type: 'string'}}
        }})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: 'bar'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: 7}")));
}

namespace {
/**
 * Helper function for parsing and creating MatchExpressions.
 */
std::unique_ptr<InternalSchemaCondMatchExpression> createCondMatchExpression(BSONObj condition,
                                                                             BSONObj thenBranch,
                                                                             BSONObj elseBranch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto conditionExpr = MatchExpressionParser::parse(condition, expCtx);
    ASSERT_OK(conditionExpr.getStatus());
    auto thenBranchExpr = MatchExpressionParser::parse(thenBranch, expCtx);
    ASSERT_OK(thenBranchExpr.getStatus());
    auto elseBranchExpr = MatchExpressionParser::parse(elseBranch, expCtx);

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        {std::move(conditionExpr.getValue()),
         std::move(thenBranchExpr.getValue()),
         std::move(elseBranchExpr.getValue())}};

    auto cond = std::make_unique<InternalSchemaCondMatchExpression>(std::move(expressions));

    return cond;
}

}  // namespace

TEST(InternalSchemaCondMatchExpressionTest, AcceptsObjectsThatMatch) {
    auto conditionQuery = BSON("age" << BSON("$lt" << 18));
    auto thenQuery = BSON("job" << "student");
    auto elseQuery = BSON("job" << "engineer");
    auto cond = createCondMatchExpression(conditionQuery, thenQuery, elseQuery);

    ASSERT_TRUE(exec::matcher::matchesBSON(cond.get(), fromjson("{age: 15, job: 'student'}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(cond.get(), fromjson("{age: 18, job: 'engineer'}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(cond.get(), fromjson("{age: [10, 20, 30], job: 'student'}")));
}

TEST(InternalSchemaCondMatchExpressionTest, RejectsObjectsThatDontMatch) {
    auto conditionQuery = BSON("age" << BSON("$lt" << 18));
    auto thenQuery = BSON("job" << "student");
    auto elseQuery = BSON("job" << "engineer");
    auto cond = createCondMatchExpression(conditionQuery, thenQuery, elseQuery);

    ASSERT_FALSE(exec::matcher::matchesBSON(cond.get(), fromjson("{age: 21, job: 'student'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(cond.get(), fromjson("{age: 5, job: 'engineer'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(cond.get(), fromjson("{age: 19}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(cond.get(), fromjson("{age: 'blah'}")));
}

TEST(InternalSchemaCondMatchExpressionTest, EmptyMatchAlwaysUsesThenBranch) {
    auto conditionQuery = BSONObj();
    auto thenQuery = BSON("value" << BSON("$gte" << 0));
    auto elseQuery = BSON("value" << BSON("$lt" << 0));
    auto cond = createCondMatchExpression(conditionQuery, thenQuery, elseQuery);

    ASSERT_TRUE(exec::matcher::matchesBSON(cond.get(), BSON("value" << 0)));
    ASSERT_TRUE(exec::matcher::matchesBSON(cond.get(), BSON("value" << 2)));

    BSONObj match = BSON("value" << 10);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(cond.get(), match.firstElement()));
}

TEST(InternalSchemaCondMatchExpressionTest, AppliesToSubobjectsViaObjectMatch) {
    auto conditionQuery = fromjson("{team: {$in: ['server', 'engineering']}}");
    auto thenQuery = BSON("subteam" << "query");
    auto elseQuery = BSON("interests" << "query optimization");

    InternalSchemaObjectMatchExpression objMatch(
        "job"_sd, createCondMatchExpression(conditionQuery, thenQuery, elseQuery));

    ASSERT_TRUE(exec::matcher::matchesBSON(
        &objMatch, fromjson("{name: 'anne', job: {team: 'engineering', subteam: 'query'}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(
        &objMatch, fromjson("{name: 'natalia', job: {team: 'server', subteam: 'query'}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(
        &objMatch,
        fromjson("{name: 'nicholas', job: {interests: ['query optimization', 'c++']}}")));

    ASSERT_FALSE(exec::matcher::matchesBSON(
        &objMatch, fromjson("{name: 'dave', team: 'server', subteam: 'query'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &objMatch, fromjson("{name: 'mateo', interests: ['perl', 'python']}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &objMatch, fromjson("{name: 'lucas', job: {team: 'competitor', subteam: 'query'}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &objMatch, fromjson("{name: 'marcos', job: {team: 'server', subteam: 'repl'}}")));
}

TEST(InternalSchemaEqMatchExpression, DoesNotTraverseThroughAnArrayWithANumericalPathComponent) {
    BSONObj operand = BSON("" << 5);
    InternalExprEqMatchExpression eq("a.0.b"_sd, operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("0" << BSON("b" << 5)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("0" << BSON("b" << 6)))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(BSON("b" << 7)))));
}

TEST(InternalSchemaEqMatchExpression, CorrectlyMatchesScalarElements) {
    BSONObj numberOperand = BSON("a" << 5);

    InternalSchemaEqMatchExpression eqNumberOperand("a"_sd, numberOperand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eqNumberOperand, BSON("a" << 5.0)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eqNumberOperand, BSON("a" << 6)));

    BSONObj stringOperand = BSON("a" << "str");

    InternalSchemaEqMatchExpression eqStringOperand("a"_sd, stringOperand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eqStringOperand, BSON("a" << "str")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eqStringOperand, BSON("a" << "string")));
}

TEST(InternalSchemaEqMatchExpression, CorrectlyMatchesArrayElement) {
    BSONObj operand = BSON("a" << BSON_ARRAY("b" << 5));

    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << 5))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(5 << "b"))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << 5 << 5))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << 6))));
}

TEST(InternalSchemaEqMatchExpression, CorrectlyMatchesNullElement) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSONNULL)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << 4)));
}

TEST(InternalSchemaEqMatchExpression, NullElementDoesNotMatchMissing) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSONObj()));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("b" << 4)));
}

TEST(InternalSchemaEqMatchExpression, NullElementDoesNotMatchUndefinedOrMissing) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSONObj()));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, fromjson("{a: undefined}")));
}

TEST(InternalSchemaEqMatchExpression, DoesNotTraverseLeafArrays) {
    BSONObj operand = BSON("a" << 5);
    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << 5.0)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(5))));
}

TEST(InternalSchemaEqMatchExpression, MatchesObjectsIndependentOfFieldOrder) {
    BSONObj operand = fromjson("{a: {b: 1, c: {d: 2, e: 3}}}");

    InternalSchemaEqMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, fromjson("{a: {b: 1, c: {d: 2, e: 3}}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, fromjson("{a: {c: {e: 3, d: 2}, b: 1}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, fromjson("{a: {b: 1, c: {d: 2}, e: 3}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, fromjson("{a: {b: 2, c: {d: 2}}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, fromjson("{a: {b: 1}}")));
}

TEST(InternalSchemaFmodMatchExpression, MatchesElement) {
    BSONObj match = BSON("a" << 1);
    BSONObj largerMatch = BSON("a" << 4.0);
    BSONObj longLongMatch = BSON("a" << 68719476736LL);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj negativeNotMatch = BSON("a" << -2);
    InternalSchemaFmodMatchExpression fmod(""_sd, Decimal128(3), Decimal128(1));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&fmod, match.firstElement()));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&fmod, largerMatch.firstElement()));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&fmod, longLongMatch.firstElement()));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&fmod, notMatch.firstElement()));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&fmod, negativeNotMatch.firstElement()));
}

TEST(InternalSchemaFmodMatchExpression, MatchesScalar) {
    InternalSchemaFmodMatchExpression fmod("a"_sd, Decimal128(5), Decimal128(2));
    ASSERT_TRUE(exec::matcher::matchesBSON(&fmod, BSON("a" << 7.0)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << 4)));
}

TEST(InternalSchemaFmodMatchExpression, MatchesNonIntegralValue) {
    InternalSchemaFmodMatchExpression fmod("a"_sd, Decimal128(10.5), Decimal128((4.5)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&fmod, BSON("a" << 15.0)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << 10.0)));
}

TEST(InternalSchemaFmodMatchExpression, MatchesArrayValue) {
    InternalSchemaFmodMatchExpression fmod("a"_sd, Decimal128(5), Decimal128(2));
    ASSERT_TRUE(exec::matcher::matchesBSON(&fmod, BSON("a" << BSON_ARRAY(5 << 12LL))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << BSON_ARRAY(6 << 8))));
}

TEST(InternalSchemaFmodMatchExpression, DoesNotMatchNull) {
    InternalSchemaFmodMatchExpression fmod("a"_sd, Decimal128(5), Decimal128(2));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSONObj()));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << BSONNULL)));
}

TEST(InternalSchemaFmodMatchExpression, NegativeRemainders) {
    InternalSchemaFmodMatchExpression fmod("a"_sd, Decimal128(5), Decimal128(-2.4));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << 7.6)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << 12.4)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&fmod, BSON("a" << Decimal128(-12.4))));
}

TEST(InternalSchemaFmodMatchExpression, ElemMatchKey) {
    InternalSchemaFmodMatchExpression fmod("a"_sd, Decimal128(5), Decimal128(2));
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT_FALSE(exec::matcher::matchesBSON(&fmod, BSON("a" << 4), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(exec::matcher::matchesBSON(&fmod, BSON("a" << 2), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(exec::matcher::matchesBSON(&fmod, BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT_TRUE(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, RejectsNonArrays) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$gt: 7}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: 'blah'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: 7}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: {i: []}}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, MatchesArraysWithMatchingElement) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$elemMatch: {'bar': 7}}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           fromjson("{foo: [[{bar: 7}], [{bar: 5}]]}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           fromjson("{foo: [[{bar: [3, 5, 7]}], [{bar: 5}]]}")));

    filter = fromjson(
        "{baz: {$_internalSchemaMatchArrayIndex:"
        "{index: 2, namePlaceholder: 'i', expression: {i: {$type: 'string'}}}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{baz: [0, 1, '2']}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, DoesNotMatchArrayIfMatchingElementNotAtIndex) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$lte: 7}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: [33, 0, 1, 2]}")));

    filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 1, namePlaceholder: 'i', expression: {i: {$lte: 7}}}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: [0, 99, 1, 2]}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, MatchesIfNotEnoughArrayElements) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{foo: []}")));

    filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 4, namePlaceholder: 'i', expression: {i: 1}}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(),
                                           fromjson("{foo: ['no', 'no', 'no', 'no']}")));
}

TEST(InternalSchemaMaxItemsMatchExpression, RejectsNonArrayElements) {
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 1);

    ASSERT(!exec::matcher::matchesBSON(&maxItems, BSON("a" << BSONObj())));
    ASSERT(!exec::matcher::matchesBSON(&maxItems, BSON("a" << 1)));
    ASSERT(!exec::matcher::matchesBSON(&maxItems, BSON("a" << "string")));
}

TEST(InternalSchemaMaxItemsMatchExpression, RejectsArraysWithTooManyElements) {
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 0);

    ASSERT(!exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(1))));
    ASSERT(!exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(1 << 2))));
}

TEST(InternalSchemaMaxItemsMatchExpression, AcceptsArrayWithLessThanOrEqualToMaxElements) {
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 2);

    ASSERT(exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(5 << 6))));
    ASSERT(exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(5))));
}

TEST(InternalSchemaMaxItemsMatchExpression, MaxItemsZeroAllowsEmptyArrays) {
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 0);

    ASSERT(exec::matcher::matchesBSON(&maxItems, BSON("a" << BSONArray())));
}

TEST(InternalSchemaMaxItemsMatchExpression, NullArrayEntriesCountAsItems) {
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 2);

    ASSERT(exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(BSONNULL << 1))));
    ASSERT(!exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(BSONNULL << 1 << 2))));
}

TEST(InternalSchemaMaxItemsMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 2);

    ASSERT(exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON_ARRAY(BSON_ARRAY(1 << 2 << 3)))));
}

TEST(InternalSchemaMaxItemsMatchExpression, NestedArraysWorkWithDottedPaths) {
    InternalSchemaMaxItemsMatchExpression maxItems("a.b"_sd, 2);

    ASSERT(exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON("b" << BSON_ARRAY(1)))));
    ASSERT(
        !exec::matcher::matchesBSON(&maxItems, BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3)))));
}

TEST(InternalSchemaMaxLengthMatchExpression, RejectsNonStringElements) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 1);

    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << BSONObj())));
    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << 1)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaMaxLengthMatchExpression, RejectsStringsWithTooManyChars) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 2);

    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << "abc")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << "abcd")));
}

TEST(InternalSchemaMaxLengthMatchExpression, AcceptsStringsWithLessThanOrEqualToMax) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&maxLength, BSON("a" << "ab")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&maxLength, BSON("a" << "a")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&maxLength, BSON("a" << "")));
}

TEST(InternalSchemaMaxLengthMatchExpression, MaxLengthZeroAllowsEmptyString) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 0);

    ASSERT_TRUE(exec::matcher::matchesBSON(&maxLength, BSON("a" << "")));
}

TEST(InternalSchemaMaxLengthMatchExpression, RejectsNull) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 1);

    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << BSONNULL)));
}

TEST(InternalSchemaMaxLengthMatchExpression, TreatsMultiByteCodepointAsOneCharacter) {
    InternalSchemaMaxLengthMatchExpression nonMatchingMaxLength("a"_sd, 0);
    InternalSchemaMaxLengthMatchExpression matchingMaxLength("a"_sd, 1);

    // This string has one code point, so it should meet maximum length 1 but not maximum length 0.
    const auto testString = u8"\U0001f4a9"_as_char_ptr;
    ASSERT_FALSE(exec::matcher::matchesBSON(&nonMatchingMaxLength, BSON("a" << testString)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&matchingMaxLength, BSON("a" << testString)));
}

TEST(InternalSchemaMaxLengthMatchExpression, CorectlyCountsUnicodeCodepoints) {
    InternalSchemaMaxLengthMatchExpression nonMatchingMaxLength("a"_sd, 4);
    InternalSchemaMaxLengthMatchExpression matchingMaxLength("a"_sd, 5);

    // A test string that contains single-byte, 2-byte, 3-byte, and 4-byte codepoints.
    const auto testString =
        u8":"                        // Single-byte character
        u8"\u00e9"                   // 2-byte character
        u8")"                        // Single-byte character
        u8"\U0001f4a9"               // 4-byte character
        u8"\U000020ac"_as_char_ptr;  // 3-byte character

    // This string has five code points, so it should meet maximum length 5 but not maximum
    // length 4.
    ASSERT_FALSE(exec::matcher::matchesBSON(&nonMatchingMaxLength, BSON("a" << testString)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&matchingMaxLength, BSON("a" << testString)));
}

TEST(InternalSchemaMaxLengthMatchExpression, DealsWithInvalidUTF8) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 1);

    // Several kinds of invalid byte sequences listed in the Wikipedia article about UTF-8:
    // https://en.wikipedia.org/wiki/UTF-8
    constexpr auto testStringUnexpectedContinuationByte = "\bf";
    constexpr auto testStringOverlongEncoding = "\xf0\x82\x82\xac";
    constexpr auto testStringInvalidCodePoint = "\xed\xa0\x80";  // U+d800 is not allowed
    constexpr auto testStringLeadingByteWithoutContinuationByte = "\xdf";

    // Because these inputs are invalid, we don't have any expectations about the answers we get.
    // Our only requirement is that the test does not crash.
    std::ignore =
        exec::matcher::matchesBSON(&maxLength, BSON("a" << testStringUnexpectedContinuationByte));
    std::ignore = exec::matcher::matchesBSON(&maxLength, BSON("a" << testStringOverlongEncoding));
    std::ignore = exec::matcher::matchesBSON(&maxLength, BSON("a" << testStringInvalidCodePoint));
    std::ignore = exec::matcher::matchesBSON(
        &maxLength, BSON("a" << testStringLeadingByteWithoutContinuationByte));
}

TEST(InternalSchemaMaxLengthMatchExpression, NestedArraysWorkWithDottedPaths) {
    InternalSchemaMaxLengthMatchExpression maxLength("a.b"_sd, 2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&maxLength, BSON("a" << BSON("b" << "a"))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&maxLength, BSON("a" << BSON("b" << "ab"))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&maxLength, BSON("a" << BSON("b" << "abc"))));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, RejectsObjectsWithTooManyElements) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(0);

    ASSERT_FALSE(exec::matcher::matchesBSON(&maxProperties, BSON("b" << 21)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&maxProperties, BSON("b" << 21 << "c" << 3)));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, AcceptsObjectWithLessThanOrEqualToMaxElements) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&maxProperties, BSONObj()));
    ASSERT_TRUE(exec::matcher::matchesBSON(&maxProperties, BSON("b" << BSONNULL)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&maxProperties, BSON("b" << 21)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&maxProperties, BSON("b" << 21 << "c" << 3)));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, MatchesSingleElementTest) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(2);

    // Only BSON elements that are embedded objects can match.
    BSONObj match = BSON("a" << BSON("a" << 5 << "b" << 10));
    BSONObj notMatch1 = BSON("a" << 1);
    BSONObj notMatch2 = BSON("a" << BSON("a" << 5 << "b" << 10 << "c" << 25));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&maxProperties, match.firstElement()));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&maxProperties, notMatch1.firstElement()));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&maxProperties, notMatch2.firstElement()));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, MaxPropertiesZeroAllowsEmptyObjects) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(0);

    ASSERT_TRUE(exec::matcher::matchesBSON(&maxProperties, BSONObj()));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, NestedObjectsAreNotUnwound) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(1);

    ASSERT_TRUE(
        exec::matcher::matchesBSON(&maxProperties, BSON("b" << BSON("c" << 2 << "d" << 3))));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&maxProperties,
                                           BSON("a" << (BSON("b" << 2 << "c" << 3 << "d" << 4)))));
}

TEST(InternalSchemaMinItemsMatchExpression, RejectsNonArrayElements) {
    InternalSchemaMinItemsMatchExpression minItems("a"_sd, 1);

    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << BSONObj())));
    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << 1)));
    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << "string")));
}

TEST(InternalSchemaMinItemsMatchExpression, RejectsArraysWithTooFewElements) {
    InternalSchemaMinItemsMatchExpression minItems("a"_sd, 2);

    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << BSONArray())));
    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaMinItemsMatchExpression, AcceptsArrayWithAtLeastMinElements) {
    InternalSchemaMinItemsMatchExpression minItems("a"_sd, 2);

    ASSERT(exec::matcher::matchesBSON(&minItems, BSON("a" << BSON_ARRAY(1 << 2))));
    ASSERT(exec::matcher::matchesBSON(&minItems, BSON("a" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(InternalSchemaMinItemsMatchExpression, MinItemsZeroAllowsEmptyArrays) {
    InternalSchemaMinItemsMatchExpression minItems("a"_sd, 0);

    ASSERT(exec::matcher::matchesBSON(&minItems, BSON("a" << BSONArray())));
}

TEST(InternalSchemaMinItemsMatchExpression, NullArrayEntriesCountAsItems) {
    InternalSchemaMinItemsMatchExpression minItems("a"_sd, 1);

    ASSERT(exec::matcher::matchesBSON(&minItems, BSON("a" << BSON_ARRAY(BSONNULL))));
    ASSERT(exec::matcher::matchesBSON(&minItems, BSON("a" << BSON_ARRAY(BSONNULL << 1))));
}

TEST(InternalSchemaMinItemsMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMinItemsMatchExpression minItems("a"_sd, 2);

    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << BSON_ARRAY(BSON_ARRAY(1 << 2)))));
}

TEST(InternalSchemaMinItemsMatchExpression, NestedArraysWorkWithDottedPaths) {
    InternalSchemaMinItemsMatchExpression minItems("a.b"_sd, 2);

    ASSERT(exec::matcher::matchesBSON(&minItems, BSON("a" << BSON("b" << BSON_ARRAY(1 << 2)))));
    ASSERT(!exec::matcher::matchesBSON(&minItems, BSON("a" << BSON("b" << BSON_ARRAY(1)))));
}

TEST(InternalSchemaMinLengthMatchExpression, RejectsNonStringElements) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 1);

    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << BSONObj())));
    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << 1)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaMinLengthMatchExpression, RejectsStringsWithTooFewChars) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 2);

    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << "")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << "a")));
}

TEST(InternalSchemaMinLengthMatchExpression, AcceptsStringWithAtLeastMinChars) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&minLength, BSON("a" << "ab")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&minLength, BSON("a" << "abc")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&minLength, BSON("a" << "abcde")));
}

TEST(InternalSchemaMinLengthMatchExpression, MinLengthZeroAllowsEmptyString) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 0);

    ASSERT_TRUE(exec::matcher::matchesBSON(&minLength, BSON("a" << "")));
}

TEST(InternalSchemaMinLengthMatchExpression, RejectsNull) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 1);

    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << BSONNULL)));
}

TEST(InternalSchemaMinLengthMatchExpression, TreatsMultiByteCodepointAsOneCharacter) {
    InternalSchemaMinLengthMatchExpression matchingMinLength("a"_sd, 1);
    InternalSchemaMinLengthMatchExpression nonMatchingMinLength("a"_sd, 2);

    // This string has one code point, so it should meet minimum length 1 but not minimum length 2.
    const auto testString = u8"\U0001f4a9"_as_char_ptr;
    ASSERT_TRUE(exec::matcher::matchesBSON(&matchingMinLength, BSON("a" << testString)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&nonMatchingMinLength, BSON("a" << testString)));
}

TEST(InternalSchemaMinLengthMatchExpression, CorectlyCountsUnicodeCodepoints) {
    InternalSchemaMinLengthMatchExpression matchingMinLength("a"_sd, 5);
    InternalSchemaMinLengthMatchExpression nonMatchingMinLength("a"_sd, 6);

    // A test string that contains single-byte, 2-byte, 3-byte, and 4-byte code points.
    const auto testString =
        u8":"                        // Single-byte character
        u8"\u00e9"                   // 2-byte character
        u8")"                        // Single-byte character
        u8"\U0001f4a9"               // 4-byte character
        u8"\U000020ac"_as_char_ptr;  // 3-byte character

    // This string has five code points, so it should meet minimum length 5 but not minimum
    // length 6.
    ASSERT_TRUE(exec::matcher::matchesBSON(&matchingMinLength, BSON("a" << testString)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&nonMatchingMinLength, BSON("a" << testString)));
}

TEST(InternalSchemaMinLengthMatchExpression, DealsWithInvalidUTF8) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 1);

    // Several kinds of invalid byte sequences listed in the Wikipedia article about UTF-8:
    // https://en.wikipedia.org/wiki/UTF-8
    constexpr auto testStringUnexpectedContinuationByte = "\bf";
    constexpr auto testStringOverlongEncoding = "\xf0\x82\x82\xac";
    constexpr auto testStringInvalidCodePoint = "\xed\xa0\x80";  // U+d800 is not allowed
    constexpr auto testStringLeadingByteWithoutContinuationByte = "\xdf";

    // Because these inputs are invalid, we don't have any expectations about the answers we get.
    // Our only requirement is that the test does not crash.
    std::ignore =
        exec::matcher::matchesBSON(&minLength, BSON("a" << testStringUnexpectedContinuationByte));
    std::ignore = exec::matcher::matchesBSON(&minLength, BSON("a" << testStringOverlongEncoding));
    std::ignore = exec::matcher::matchesBSON(&minLength, BSON("a" << testStringInvalidCodePoint));
    std::ignore = exec::matcher::matchesBSON(
        &minLength, BSON("a" << testStringLeadingByteWithoutContinuationByte));
}

TEST(InternalSchemaMinLengthMatchExpression, NestedFieldsWorkWithDottedPaths) {
    InternalSchemaMinLengthMatchExpression minLength("a.b"_sd, 2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&minLength, BSON("a" << BSON("b" << "ab"))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&minLength, BSON("a" << BSON("b" << "abc"))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&minLength, BSON("a" << BSON("b" << "a"))));
}

TEST(InternalSchemaMinPropertiesMatchExpression, RejectsObjectsWithTooFewElements) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_FALSE(exec::matcher::matchesBSON(&minProperties, BSONObj()));
    ASSERT_FALSE(exec::matcher::matchesBSON(&minProperties, BSON("b" << 21)));
}


TEST(InternalSchemaMinPropertiesMatchExpression, AcceptsObjectWithAtLeastMinElements) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_TRUE(exec::matcher::matchesBSON(&minProperties, BSON("b" << 21 << "c" << BSONNULL)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&minProperties, BSON("b" << 21 << "c" << 3)));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&minProperties, BSON("b" << 21 << "c" << 3 << "d" << 43)));
}

TEST(InternalSchemaMinPropertiesMatchExpression, MatchesSingleElementTest) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    // Only BSON elements that are embedded objects can match.
    BSONObj match = BSON("a" << BSON("a" << 5 << "b" << 10));
    BSONObj notMatch1 = BSON("a" << 1);
    BSONObj notMatch2 = BSON("a" << BSON("b" << 10));

    ASSERT_TRUE(exec::matcher::matchesSingleElement(&minProperties, match.firstElement()));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&minProperties, notMatch1.firstElement()));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&minProperties, notMatch2.firstElement()));
}

TEST(InternalSchemaMinPropertiesMatchExpression, MinPropertiesZeroAllowsEmptyObjects) {
    InternalSchemaMinPropertiesMatchExpression minProperties(0);

    ASSERT_TRUE(exec::matcher::matchesBSON(&minProperties, BSONObj()));
}

TEST(InternalSchemaMinPropertiesMatchExpression, NestedObjectsAreNotUnwound) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_FALSE(
        exec::matcher::matchesBSON(&minProperties, BSON("b" << BSON("c" << 2 << "d" << 3))));
}

TEST(InternalSchemaMinPropertiesMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_FALSE(exec::matcher::matchesBSON(&minProperties,
                                            BSON("a" << (BSON("b" << 2 << "c" << 3 << "d" << 4)))));
}

TEST(InternalSchemaObjectMatchExpression, RejectsNonObjectElements) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b" << 1), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << 1)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << "string")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON_ARRAY(BSONNULL))));
}

TEST(InternalSchemaObjectMatchExpression, RejectsObjectsThatDontMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b" << BSON("$type" << "string")), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON("b" << 1))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON("b" << BSONObj()))));
}

TEST(InternalSchemaObjectMatchExpression, AcceptsObjectsThatMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b" << BSON("$type" << "string")), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_TRUE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON("b" << "string"))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&objMatch,
                                           BSON("a" << BSON("b" << "string"
                                                                << "c" << 1))));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &objMatch, BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << "string")))));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON("b" << BSON_ARRAY("string")))));
}

TEST(InternalSchemaObjectMatchExpression, DottedPathAcceptsObjectsThatMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b.c.d" << BSON("$type" << "string")), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON("d" << "string"))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch,
                                            BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1))))));
    ASSERT_TRUE(exec::matcher::matchesBSON(
        &objMatch, BSON("a" << BSON("b" << BSON("c" << BSON("d" << "foo"))))));
}

TEST(InternalSchemaObjectMatchExpression, EmptyMatchAcceptsAllObjects) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSONObj(), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << 1)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << "string")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSONObj())));
    ASSERT_TRUE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON("b" << "string"))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&objMatch, BSON("a" << BSON_ARRAY(BSONObj()))));
}

TEST(InternalSchemaObjectMatchExpression, MatchesNestedObjectMatch) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "       b: {$_internalSchemaObjectMatch: {"
        "           c: 3"
        "       }}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_FALSE(exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: {b: {c: 1}}}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: {b: {c: 3}}}")));
}

TEST(InternalSchemaObjectMatchExpression, SubExpressionRespectsCollator) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "	b: {$eq: 'FOO'}"
        "}}}");
    auto objectMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objectMatch.getStatus());

    ASSERT_TRUE(
        exec::matcher::matchesBSON(objectMatch.getValue().get(), fromjson("{a: {b: 'FOO'}}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(objectMatch.getValue().get(), fromjson("{a: {b: 'foO'}}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(objectMatch.getValue().get(), fromjson("{a: {b: 'foo'}}")));
}

TEST(InternalSchemaObjectMatchExpression, RejectsArraysContainingMatchingSubObject) {
    auto query = fromjson("{a: {$_internalSchemaObjectMatch: {b: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_FALSE(exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: 1}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: [{b: 1}]}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(objMatch.getValue().get(), fromjson("{a: [{b: 1}, {b: 2}]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, RejectsNonArrays) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_FALSE(exec::matcher::matchesBSON(&uniqueItems, BSON("foo" << 1)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&uniqueItems, BSON("foo" << BSONObj())));
    ASSERT_FALSE(exec::matcher::matchesBSON(&uniqueItems, BSON("foo" << "string")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesEmptyArray) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems, BSON("foo" << BSONArray())));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesOneElementArray) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems, BSON("foo" << BSON_ARRAY(1))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems, BSON("foo" << BSON_ARRAY(BSONObj()))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems,
                                           BSON("foo" << BSON_ARRAY(BSON_ARRAY(9 << "bar")))));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesArrayOfUniqueItems) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [1, 'bar', {}, [], null]}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems,
                                           fromjson("{foo: [{x: 1}, {x: 2}, {x: 2, y: 3}]}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [[1], [1, 2], 1]}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [['a', 'b'], ['b', 'a']]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, MatchesNestedArrayOfUniqueItems) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo.bar"_sd);
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems,
                                           fromjson("{foo: {bar: [1, 'bar', {}, [], null]}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(
        &uniqueItems, fromjson("{foo: {bar: [{x: 1}, {x: 2}, {x: 2, y: 3}]}}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: {bar: [[1], [1, 2], 1]}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems,
                                           fromjson("{foo: {bar: [['a', 'b'], ['b', 'a']]}}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, RejectsArrayWithDuplicates) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_FALSE(exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [1, 1, 1]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [['bar'], ['bar']]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &uniqueItems, fromjson("{foo: [{x: 'a', y: [1, 2]}, {y: [1, 2], x: 'a'}]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, RejectsNestedArrayWithDuplicates) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_FALSE(exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: {bar: [1, 1, 1]}}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: {bar: [['baz'], ['baz']]}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &uniqueItems, fromjson("{foo: {bar: [{x: 'a', y: [1, 2]}, {y: [1, 2], x: 'a'}]}}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, FieldNameSignificantWhenComparingNestedObjects) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [{x: 7}, {y: 7}]}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [{a: 'bar'}, {b: 'bar'}]}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [{a: 'bar'}, {a: 'bar'}]}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [[1, 2, 3], [1, 3, 2]]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, AlwaysUsesBinaryComparisonRegardlessOfCollator) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    uniqueItems.setCollator(&collator);

    ASSERT_TRUE(exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: ['one', 'OnE', 'ONE']}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [{x: 'two'}, {y: 'two'}]}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&uniqueItems, fromjson("{foo: [{a: 'three'}, {a: 'THREE'}]}")));
}

TEST(InternalSchemaUniqueItemsMatchExpression, FindsFirstDuplicateValue) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems(""_sd);
    auto inputArray = fromjson("[1, 2, 2, 1]");
    auto result = exec::matcher::findFirstDuplicateValue(&uniqueItems, inputArray);
    ASSERT_TRUE(result);
    ASSERT_EQUALS(result.Int(), 2);
    ASSERT_FALSE(exec::matcher::findFirstDuplicateValue(&uniqueItems, fromjson("[1, 2]")));
    ASSERT_FALSE(exec::matcher::findFirstDuplicateValue(&uniqueItems, fromjson("[]")));
}

TEST(InternalSchemaXorOp, MatchesNothingWhenHasNoClauses) {
    InternalSchemaXorMatchExpression internalSchemaXorOp;
    ASSERT_FALSE(exec::matcher::matchesBSON(&internalSchemaXorOp, BSONObj()));
}

TEST(InternalSchemaXorOp, MatchesSingleClause) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: { $ne: 5 }}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 4)));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << BSON_ARRAY(4 << 6))));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 5)));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << BSON_ARRAY(4 << 5))));
}

TEST(InternalSchemaXorOp, MatchesThreeClauses) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj matchPredicate =
        fromjson("{$_internalSchemaXor: [{a: { $gt: 10 }}, {a: { $lt: 0 }}, {b: 0}]}");

    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << -1)));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 11)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 5)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("b" << 100)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("b" << 101)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSONObj()));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 11 << "b" << 100)));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 11 << "b" << 0)));
}

TEST(InternalSchemaXorOp, MatchesSingleElement) {
    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: {$lt: 5 }}, {b: 10}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);

    ASSERT_OK(expr.getStatus());
    BSONObj match1 = BSON("a" << 4);
    BSONObj match2 = BSON("b" << 10);
    BSONObj noMatch1 = BSON("a" << 8);
    BSONObj noMatch2 = BSON("c" << "x");

    ASSERT_TRUE(exec::matcher::matchesSingleElement(expr.getValue().get(), match1.firstElement()));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(expr.getValue().get(), match2.firstElement()));
    ASSERT_FALSE(
        exec::matcher::matchesSingleElement(expr.getValue().get(), noMatch1.firstElement()));
    ASSERT_FALSE(
        exec::matcher::matchesSingleElement(expr.getValue().get(), noMatch2.firstElement()));
}

TEST(InternalSchemaXorOp, DoesNotUseElemMatchKey) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    BSONObj matchPredicate = fromjson("{$_internalSchemaXor: [{a: 1}, {b: 2}]}");

    auto expr = MatchExpressionParser::parse(matchPredicate, expCtx);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 1), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_FALSE(exec::matcher::matchesBSON(
        expr.getValue().get(), BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << 2.0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MinItemsCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinItems" << Decimal128("2")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}


TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << 2.0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, MaxItemsCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxItems" << Decimal128("2")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserSchemaTest, UniqueItemsParsesTrueBooleanArgument) {
    auto query = BSON("x" << BSON("$_internalSchemaUniqueItems" << true));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: 'blah'}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: []}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: [0]}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: ['7', null, [], {}, 7]}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: ['dup', 'dup', 7]}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: [{x: 1}, {x: 1}]}")));
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchCorrectlyParsesObjects) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    b: {$gte: 0}"
        "    }}"
        "}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: 1}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: 'string'}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: -1}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: [{b: 0}]}")));
}

TEST(MatchExpressionParserSchemaTest, ObjectMatchCorrectlyParsesNestedObjectMatch) {
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "    b: {$_internalSchemaObjectMatch: {"
        "        $or: [{c: {$type: 'string'}}, {c: {$gt: 0}}]"
        "    }}"
        "}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: 1}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: {c: {}}}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: {c: 0}}}")));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: {c: 'string'}}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {b: {c: 1}}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        result.getValue().get(), fromjson("{a: [{b: 0}, {b: [{c: 0}, {c: 'string'}]}]}")));
}

//
// Tests for parsing the $_internalSchemaMinLength expression.
//
TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << 2.0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MinLengthCorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMinLength" << Decimal128("2")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

//
// Tests for parsing the $_internalSchemaMaxLength expression.
//
TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesIntegerArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesLongArgument) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthCorrectlyParsesDoubleArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << 2.0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, MaxLengthorrectlyParsesDecimalArgumentAsInteger) {
    BSONObj query = BSON("x" << BSON("$_internalSchemaMaxLength" << Decimal128("2")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ab")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
}

TEST(MatchExpressionParserSchemaTest, CondParsesThreeMatchExpressions) {
    auto query = fromjson(
        "{$_internalSchemaCond: [{climate: 'rainy'}, {clothing: 'jacket'}, {clothing: 'shirt'}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(
        expr.getValue().get(), fromjson("{climate: 'rainy', clothing: ['jacket', 'umbrella']}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(
        expr.getValue().get(), fromjson("{climate: 'sunny', clothing: ['shirt', 'shorts']}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(),
                                            fromjson("{climate: 'rainy', clothing: ['poncho']}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{clothing: ['jacket']}")));
}

TEST(MatchExpressionParserSchemaTest, MatchArrayIndexParsesSuccessfully) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$lt: 0}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchArrayIndex = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(matchArrayIndex.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(matchArrayIndex.getValue().get(),
                                           fromjson("{foo: [-1, 0, 1]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(matchArrayIndex.getValue().get(),
                                            fromjson("{foo: [2, 'blah']}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(matchArrayIndex.getValue().get(),
                                            fromjson("{foo: [{x: 'baz'}]}")));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, ParsesCorrectlyWithValidInput) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: { $lt: 4 }}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: [5, 3, 3, 3, 3, 3]}")));
    ASSERT_FALSE(
        exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{a: [3, 3, 3, 5, 3, 3]}")));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesParsesSuccessfully) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['phoneNumber', 'address'],"
        "namePlaceholder: 'i', otherwise: {i: {$gt: 10}},"
        "patternProperties: [{regex: /[nN]umber/, expression: {i: {$type: 'number'}}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto allowedProperties = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(allowedProperties.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(allowedProperties.getValue().get(),
                                           fromjson("{phoneNumber: 123, address: 'earth'}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(allowedProperties.getValue().get(),
                                           fromjson("{phoneNumber: 3.14, workNumber: 456}")));

    ASSERT_FALSE(exec::matcher::matchesBSON(allowedProperties.getValue().get(),
                                            fromjson("{otherNumber: 'blah'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(allowedProperties.getValue().get(),
                                            fromjson("{phoneNumber: 'blah'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(allowedProperties.getValue().get(),
                                            fromjson("{other: 'blah'}")));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesAcceptsEmptyPropertiesAndPatternProperties) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: {i: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto allowedProperties = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(allowedProperties.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(allowedProperties.getValue().get(), BSONObj()));
}

TEST(MatchExpressionParserSchemaTest, AllowedPropertiesAcceptsEmptyOtherwiseExpression) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties:"
        "{properties: [], namePlaceholder: 'i', patternProperties: [], otherwise: {}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto allowedProperties = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(allowedProperties.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(allowedProperties.getValue().get(), BSONObj()));
}

TEST(MatchExpressionParserSchemaTest, EqParsesSuccessfully) {
    auto query = fromjson("{foo: {$_internalSchemaEq: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto eqExpr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(eqExpr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(eqExpr.getValue().get(), fromjson("{foo: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(eqExpr.getValue().get(), fromjson("{foo: [1]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(eqExpr.getValue().get(), fromjson("{not_foo: 1}")));
}

TEST(MatchExpressionParserSchemaTest, RootDocEqParsesSuccessfully) {
    auto query = fromjson("{$_internalSchemaRootDocEq: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto eqExpr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(eqExpr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(eqExpr.getValue().get(), fromjson("{}")));
}

TEST(InternalBinDataSubTypeMatchExpressionTest, SubTypeParsesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataSubType" << BinDataType::bdtCustom));
    auto statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());

    uint8_t bytes[] = {0, 1, 2, 3, 4, 5};
    BSONObj match = BSON("a" << BSONBinData(bytes, 5, BinDataType::bdtCustom));
    BSONObj notMatch = BSON("a" << BSONBinData(bytes, 5, BinDataType::Function));

    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), match));
    ASSERT_FALSE(exec::matcher::matchesBSON(statusWith.getValue().get(), notMatch));
}

TEST(InternalBinDataSubTypeMatchExpressionTest, SubTypeWithFloatParsesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataSubType" << 5.0));
    auto statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());

    uint8_t bytes[] = {0, 1, 2, 3, 4, 5};
    BSONObj match = BSON("a" << BSONBinData(bytes, 5, BinDataType::MD5Type));
    BSONObj notMatch = BSON("a" << BSONBinData(bytes, 5, BinDataType::bdtCustom));

    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), match));
    ASSERT_FALSE(exec::matcher::matchesBSON(statusWith.getValue().get(), notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeTest, DoesNotTraverseLeafArrays) {
    MatcherTypeSet typeSet;
    typeSet.bsonTypes.insert(BSONType::string);
    typeSet.bsonTypes.insert(BSONType::date);
    InternalSchemaBinDataEncryptedTypeExpression expr("a"_sd, std::move(typeSet));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);
    auto binData = BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, BSON("a" << binData)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, BSON("a" << BSON_ARRAY(binData))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, BSON("a" << BSONArray())));
}

TEST(InternalSchemaBinDataEncryptedTypeTest, DoesNotMatchShortBinData) {
    MatcherTypeSet typeSet;
    typeSet.bsonTypes.insert(BSONType::string);
    typeSet.bsonTypes.insert(BSONType::date);
    InternalSchemaBinDataEncryptedTypeExpression expr("a"_sd, std::move(typeSet));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);
    auto binData = BSONBinData(reinterpret_cast<const void*>(&blob),
                               sizeof(FleBlobHeader) - sizeof(blob.originalBsonType),
                               BinDataType::Encrypt);

    ASSERT_FALSE(exec::matcher::matchesBSON(&expr,
                                            BSON("a" << binData << "foo"
                                                     << "bar")));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeMatchesSingleTypeAlias) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << "string"));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), matchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeMatchesSingleType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), matchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeMatchesOneOfTypesInArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                  << BSON_ARRAY(BSONType::date << BSONType::string)));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), matchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeDoesNotMatchSingleType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::numberInt);

    BSONObj notMatchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                     sizeof(FleBlobHeader),
                                                     BinDataType::Encrypt));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), notMatchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, BsonTypeDoesNotMatchTypeArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType"
                                  << BSON_ARRAY(BSONType::date << BSONType::boolean)));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kDeterministic);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::numberInt);

    BSONObj notMatchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                     sizeof(FleBlobHeader),
                                                     BinDataType::Encrypt));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), notMatchingDoc));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, IntentToEncryptFleBlobDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kPlaceholder);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);
    BSONObj notMatch = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                               sizeof(FleBlobHeader),
                                               BinDataType::Encrypt));

    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, UnknownFleBlobTypeDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    FleBlobHeader blob;
    blob.fleBlobSubtype = 6;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);
    BSONObj notMatch = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                               sizeof(FleBlobHeader),
                                               BinDataType::Encrypt));
    try {
        exec::matcher::matchesBSON(expr.get(), notMatch);
    } catch (...) {
        ASSERT_EQ(exceptionToStatus().code(), 33118);
    }
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, EmptyFleBlobDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Encrypt));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, NonEncryptBinDataSubTypeDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    BSONObj notMatch = BSON("a" << BSONBinData("\x69\xb7", 2, BinDataGeneral));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), notMatch));
}

TEST(InternalSchemaBinDataEncryptedTypeExpressionTest, NonBinDataValueDoesNotMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto query = BSON("a" << BSON("$_internalSchemaBinDataEncryptedType" << BSONType::string));
    auto expr = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));

    BSONObj notMatch = BSON("a" << BSONArray());
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), notMatch));
}

TEST(InternalSchemaBinDataFLE2EncryptedTypeTest, DoesNotTraverseLeafArrays) {
    InternalSchemaBinDataFLE2EncryptedTypeExpression expr("a"_sd, BSONType::string);

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValue);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    auto binData = BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);

    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, BSON("a" << binData)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, BSON("a" << BSON_ARRAY(binData))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, BSON("a" << BSONArray())));
}

TEST(InternalSchemaBinDataFLE2EncryptedTypeTest, DoesNotMatchShortBinData) {
    InternalSchemaBinDataFLE2EncryptedTypeExpression expr("a"_sd, BSONType::string);

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValue);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    auto binData = BSONBinData(reinterpret_cast<const void*>(&blob),
                               sizeof(FleBlobHeader) - sizeof(blob.originalBsonType),
                               BinDataType::Encrypt);
    auto emptyBinData = BSONBinData(reinterpret_cast<const void*>(&blob), 0, BinDataType::Encrypt);

    ASSERT_FALSE(exec::matcher::matchesBSON(&expr,
                                            BSON("a" << binData << "foo"
                                                     << "bar")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, BSON("a" << emptyBinData)));
}

TEST(InternalSchemaBinDataFLE2EncryptedTypeTest, MatchesOnlyFLE2ServerSubtypes) {
    InternalSchemaBinDataFLE2EncryptedTypeExpression expr("a"_sd, BSONType::string);

    FleBlobHeader blob;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    for (uint8_t i = 0; i < idlEnumCount<EncryptedBinDataType>; i++) {
        blob.fleBlobSubtype = i;
        auto binData = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);

        if (i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValue) ||
            i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2RangeIndexedValue) ||
            i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValueV2) ||
            i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2RangeIndexedValueV2) ||
            i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValue) ||
            i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2) ||
            i == static_cast<uint8_t>(EncryptedBinDataType::kFLE2TextIndexedValue)) {
            ASSERT_TRUE(exec::matcher::matchesBSON(&expr, BSON("a" << binData)));
        } else {
            ASSERT_FALSE(exec::matcher::matchesBSON(&expr, BSON("a" << binData)));
        }
    }
}

TEST(InternalSchemaBinDataFLE2EncryptedTypeTest, DoesNotMatchIncorrectBsonType) {
    InternalSchemaBinDataFLE2EncryptedTypeExpression encryptedString("ssn"_sd, BSONType::string);
    InternalSchemaBinDataFLE2EncryptedTypeExpression encryptedInt("age"_sd, BSONType::numberInt);

    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<uint8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValue);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = stdx::to_underlying(BSONType::string);

    auto binData = BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
    ASSERT_TRUE(exec::matcher::matchesBSON(&encryptedString, BSON("ssn" << binData)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&encryptedInt, BSON("age" << binData)));

    blob.originalBsonType = stdx::to_underlying(BSONType::numberInt);
    binData = BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
    ASSERT_FALSE(exec::matcher::matchesBSON(&encryptedString, BSON("ssn" << binData)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&encryptedInt, BSON("age" << binData)));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesObject) {
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << 1 << "b"
                                                              << "string"));
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq,
                                           BSON("a" << 1 << "b"
                                                    << "string")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&rootDocEq,
                                            BSON("a" << 2 << "b"
                                                     << "string")));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesNestedObject) {
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << 1 << "b" << BSON("c" << 1)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq, BSON("a" << 1 << "b" << BSON("c" << 1))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&rootDocEq, BSON("a" << 1 << "b" << BSON("c" << 2))));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesObjectIgnoresElementOrder) {
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << 1 << "b" << BSON("c" << 1)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq, BSON("b" << BSON("c" << 1) << "a" << 1)));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesNestedObjectIgnoresElementOrder) {
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << BSON("b" << 1 << "c" << 1)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq, BSON("a" << BSON("c" << 1 << "b" << 1))));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesEmptyObject) {
    InternalSchemaRootDocEqMatchExpression rootDocEq{BSONObj()};
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq, BSONObj()));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesNestedArray) {
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << BSON_ARRAY(1 << 2 << 3)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq, BSON("a" << BSON_ARRAY(1 << 2 << 3))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&rootDocEq, BSON("a" << BSON_ARRAY(1 << 3 << 2))));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesObjectWithNullElement) {
    InternalSchemaRootDocEqMatchExpression rootDocEq(fromjson("{a: null}"));
    ASSERT_TRUE(exec::matcher::matchesBSON(&rootDocEq, fromjson("{a: null}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&rootDocEq, fromjson("{a: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&rootDocEq, fromjson("{}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&rootDocEq, fromjson("{a: undefined}")));
}

}  // namespace mongo::evaluate_internal_schema_matcher_test
