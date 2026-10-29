// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
// Helper function to extract the original $expr expression from the optimized $and expression.
const MatchExpression* getExpr(const MatchExpression& origExpr) {
    if (origExpr.matchType() == MatchExpression::EXPRESSION) {
        return &origExpr;
    }
    ASSERT_TRUE(origExpr.matchType() == MatchExpression::AND);
    auto exprExpr = origExpr.getChild(0);
    ASSERT_TRUE(exprExpr->matchType() == MatchExpression::EXPRESSION);
    return exprExpr;
}

void checkExprTranslationToIndexBounds(std::string indexedField,
                                       bool simpleIndex,
                                       std::string exprString,
                                       size_t intervalSize) {
    BSONObj keyPattern = BSON(indexedField << 1);
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = simpleIndex
        ? IndexBoundsBuilderTest::buildSimpleIndexEntry(keyPattern)
        : IndexBoundsBuilderTest::buildMultikeyIndexEntry(keyPattern, {{0U}, {}});

    BSONObj obj = fromjson(exprString);
    auto [expr, inputParamIdMap] = IndexBoundsBuilderTest::parseMatchExpression(obj);
    auto exprExpr = getExpr(*expr);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(exprExpr, elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.name, indexedField);
    ASSERT_EQUALS(oil.intervals.size(), intervalSize);
    if (intervalSize > 0) {
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
        IndexBoundsBuilderTest::assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
    }
}

TEST_F(IndexBoundsBuilderTest, TranslateExprWithEqualityToConstant) {
    checkExprTranslationToIndexBounds(
        "a", true /* simple index */, "{$expr: {$eq: ['$a', '1']}}", 1U);
    checkExprTranslationToIndexBounds(
        "a", true /* simple index */, "{$expr: {$eq: ['1', '$a']}}", 1U);
}

TEST_F(IndexBoundsBuilderTest, DoesNotTranslateExpr) {
    // Equality to null.
    checkExprTranslationToIndexBounds(
        "a", true /* simple index */, "{$expr: {$eq: ['$a', null]}}", 0U);
    // Inequality.
    checkExprTranslationToIndexBounds(
        "a", true /* simple index */, "{$expr: {$lte: ['$a', 10]}}", 0U);
    // Equality to path.
    checkExprTranslationToIndexBounds(
        "a", true /* simple index */, "{$expr: {$eq: ['$a', '$b']}}", 0U);
}

TEST_F(IndexBoundsBuilderTest, DoesNotTranslateExprWithMultikeyIndex) {
    checkExprTranslationToIndexBounds(
        "a", false /* simple index */, "{$expr: {$eq: ['$a', 3]}}", 0U);
}

using IndexBoundsBuilderTestDeathTest = IndexBoundsBuilderTest;
DEATH_TEST_F(IndexBoundsBuilderTestDeathTest,
             TranslateExprWithHashedIndexFails,
             "Translating unsupported predicate for a hashed index") {
    BSONObj keyPattern = BSON("a" << "hashed");
    BSONElement elt = keyPattern.firstElement();
    auto testIndex = buildSimpleIndexEntry(keyPattern);
    BSONObj obj = fromjson("{$expr: {$eq: ['$a', 3]}}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    auto exprExpr = getExpr(*expr);

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(exprExpr, elt, testIndex, &oil, &tightness, &ietBuilder);
}
}  // namespace mongo
