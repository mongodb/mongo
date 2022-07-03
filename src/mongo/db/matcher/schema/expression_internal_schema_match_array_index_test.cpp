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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(InternalSchemaMatchArrayIndexMatchExpression, RejectsNonArrays) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$gt: 7}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{foo: 'blah'}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{foo: 7}")));
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{foo: {i: []}}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, MatchesArraysWithMatchingElement) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$elemMatch: {'bar': 7}}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{foo: [[{bar: 7}], [{bar: 5}]]}")));
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{foo: [[{bar: [3, 5, 7]}], [{bar: 5}]]}")));

    filter = fromjson(
        "{baz: {$_internalSchemaMatchArrayIndex:"
        "{index: 2, namePlaceholder: 'i', expression: {i: {$type: 'string'}}}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{baz: [0, 1, '2']}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, DoesNotMatchArrayIfMatchingElementNotAtIndex) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$lte: 7}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{foo: [33, 0, 1, 2]}")));

    filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 1, namePlaceholder: 'i', expression: {i: {$lte: 7}}}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(expr.getValue()->matchesBSON(fromjson("{foo: [0, 99, 1, 2]}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, MatchesIfNotEnoughArrayElements) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{foo: []}")));

    filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 4, namePlaceholder: 'i', expression: {i: 1}}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(fromjson("{foo: ['no', 'no', 'no', 'no']}")));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, EquivalentToClone) {
    auto filter = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$type: 'number'}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    auto clone = expr.getValue()->shallowClone();
    ASSERT_TRUE(expr.getValue()->equivalent(clone.get()));
}

TEST(InternalSchemaMatchArrayIndexMatchExpression, HasSingleChild) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$type: 'number'}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 1U);
    ASSERT(objMatch.getValue()->getChild(0));
}

DEATH_TEST_REGEX(InternalSchemaMatchArrayIndexMatchExpression,
                 GetChildFailsIndexGreaterThanOne,
                 "Tripwire assertion.*6400214") {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$type: 'number'}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 1);
    ASSERT_THROWS_CODE(objMatch.getValue()->getChild(1), AssertionException, 6400214);
}
}  // namespace
}  // namespace mongo
