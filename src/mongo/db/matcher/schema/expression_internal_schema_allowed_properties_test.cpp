// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

TEST(InternalSchemaAllowedPropertiesMatchExpression, EquivalentToClone) {
    auto filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {i: 1}}], otherwise: {i: 7}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    auto clone = expr.getValue()->clone();
    ASSERT_TRUE(expr.getValue()->equivalent(clone.get()));

    filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [], otherwise: {}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    clone = expr.getValue()->clone();
    ASSERT_TRUE(expr.getValue()->equivalent(clone.get()));
}

TEST(InternalSchemaAllowedPropertiesMatchExpression, HasCorrectNumberOfChilden) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {i: 1}}], otherwise: {i: 7}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 2U);
    ASSERT(objMatch.getValue()->getChild(0));
}

DEATH_TEST_REGEX(InternalSchemaAllowedPropertiesMatchExpressionDeathTest,
                 GetChildFailsOnIndexLargerThanChildSet,
                 "Tripwire assertion.*6400212") {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {i: 1}}], otherwise: {i: 7}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    const size_t numChildren = 2;
    ASSERT_EQ(objMatch.getValue()->numChildren(), numChildren);
    ASSERT_THROWS_CODE(objMatch.getValue()->getChild(numChildren), AssertionException, 6400212);
}
}  // namespace mongo
