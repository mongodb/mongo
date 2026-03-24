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
