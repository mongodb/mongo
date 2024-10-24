/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/cost_based_ranker/heuristic_estimator.h"

#include <limits>

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::cost_based_ranker {
namespace {

CardinalityEstimate makeCard(double d) {
    return CardinalityEstimate(CardinalityType(d), EstimationSource::Code);
}

SelectivityEstimate makeSel(double d) {
    return SelectivityEstimate(SelectivityType(d), EstimationSource::Code);
}

std::unique_ptr<MatchExpression> parse(const BSONObj& bson) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto expr = MatchExpressionParser::parse(
        bson, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(expr);
    return std::move(expr.getValue());
}

TEST(HeuristicEstimator, PointInterval) {
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(5));
    IndexBounds bounds;
    bounds.fields.push_back(oil);
    ASSERT_EQ(estimateIndexBounds(bounds, makeCard(100)), makeSel(0.1));
}

TEST(HeuristicEstimator, ManyPointIntervals) {
    OrderedIntervalList oil;
    for (size_t i = 0; i < 100; ++i) {
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i));
    }
    IndexBounds bounds;
    bounds.fields.push_back(oil);
    ASSERT_EQ(estimateIndexBounds(bounds, makeCard(100)), makeSel(0.179262));
}

TEST(HeuristicEstimator, CompoundIndex) {
    IndexBounds bounds;
    for (size_t i = 0; i < 10; ++i) {
        OrderedIntervalList oil;
        for (size_t j = 0; j < 10; ++j) {
            oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i * j));
        }
        bounds.fields.push_back(oil);
    }
    ASSERT_EQ(estimateIndexBounds(bounds, makeCard(100)), makeSel(0.0398372));
}

TEST(HeuristicEstimator, PointMoreSelectiveThanRange) {
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(5));
    IndexBounds pointBounds;
    pointBounds.fields.push_back(oil);

    OrderedIntervalList oilRange;
    oilRange.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 5 << " " << 6), BoundInclusion::kIncludeBothStartAndEndKeys));
    IndexBounds rangeBounds;
    rangeBounds.fields.push_back(oilRange);

    ASSERT_LT(estimateIndexBounds(pointBounds, makeCard(100)),
              estimateIndexBounds(rangeBounds, makeCard(100)));
}

TEST(HeuristicEstimator, CompoundBoundsMoreSelectiveThanSingleField) {
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(5));
    IndexBounds singleField;
    singleField.fields.push_back(oil);

    IndexBounds compoundBounds;
    compoundBounds.fields.push_back(oil);
    compoundBounds.fields.push_back(oil);

    ASSERT_LT(estimateIndexBounds(compoundBounds, makeCard(100)),
              estimateIndexBounds(singleField, makeCard(100)));
}

TEST(HeuristicEstimator, PointIntervalSelectivityDependsOnInputCard) {
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(5));
    IndexBounds bounds;
    bounds.fields.push_back(oil);
    ASSERT_LT(estimateIndexBounds(bounds, makeCard(10000)),
              estimateIndexBounds(bounds, makeCard(100)));
}

TEST(HeuristicEstimator, AlwaysFalse) {
    BSONObj query = fromjson("{$alwaysFalse: 1}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), zeroSel);
}

TEST(HeuristicEstimator, AlwaysTrue) {
    BSONObj query = fromjson("{$alwaysTrue: 1}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), oneSel);
}

TEST(HeuristicEstimator, EqualityMatchesIndexPointInterval) {
    // Bounds for [5,5]
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(5));
    IndexBounds bounds;
    bounds.fields.push_back(oil);

    // Expression for a = 5
    BSONObj query = fromjson("{a: 5}");
    auto expr = parse(query);

    auto collCard = makeCard(100);
    ASSERT_EQ(estimateIndexBounds(bounds, collCard),
              estimateLeafMatchExpression(expr.get(), collCard));
}

TEST(HeuristicEstimate, InequalityMatchesRangeOpenInterval) {
    // Bounds for (5,inf]
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 5 << " " << std::numeric_limits<double>::infinity()),
        BoundInclusion::kIncludeEndKeyOnly));
    IndexBounds bounds;
    bounds.fields.push_back(oil);

    // Expression for a > 5
    BSONObj query = fromjson("{a: {$gt: 5}}");
    auto expr = parse(query);

    auto collCard = makeCard(100);
    ASSERT_EQ(estimateIndexBounds(bounds, collCard),
              estimateLeafMatchExpression(expr.get(), collCard));
}

TEST(HeuristicEstimate, InequalityMatchesRangeClosedInterval) {
    // Bounds for [5,inf]
    OrderedIntervalList oil;
    oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 5 << " " << std::numeric_limits<double>::infinity()),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    IndexBounds bounds;
    bounds.fields.push_back(oil);

    // Expression for a >= 5
    BSONObj query = fromjson("{a: {$gte: 5}}");
    auto expr = parse(query);

    auto collCard = makeCard(100);
    ASSERT_EQ(estimateIndexBounds(bounds, collCard),
              estimateLeafMatchExpression(expr.get(), collCard));
}

TEST(HeuristicEstimate, RegexMatchExpression) {
    BSONObj query = fromjson("{a: /abc/}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), kRegexSel);
}

TEST(HeuristicEstimate, ModExpression) {
    BSONObj query = fromjson("{a: {$mod: [5, 2]}}");
    auto expr = parse(query);
    // Selectivty of mod 5 is 1/5
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), makeSel(0.2));
}

TEST(HeuristicEstimate, ExistsExpression) {
    // Note: {$exists: false} is parsed as {$not: {$exists: true}}
    BSONObj query = fromjson("{a: {$exists: true}}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), kExistsSel);
}

TEST(HeuristicEstimate, InExpressionMatchesIntervals) {
    // Interval for [[1,1], [2,2], [3,3]]
    OrderedIntervalList oil;
    for (size_t i = 0; i < 3; ++i) {
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i));
    }
    IndexBounds bounds;
    bounds.fields.push_back(oil);

    BSONObj query = fromjson("{a: {$in: [1,2,3]}}");
    auto expr = parse(query);

    auto collCard = makeCard(100);
    ASSERT_EQ(estimateIndexBounds(bounds, collCard),
              estimateLeafMatchExpression(expr.get(), collCard));
}

TEST(HeuristicEstimate, TypeExpressionMatchesIntervals) {
    OrderedIntervalList oil;
    oil.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << Date_t::min() << " " << Date_t::max()),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << false << " " << true), BoundInclusion::kIncludeBothStartAndEndKeys));
    IndexBounds bounds;
    bounds.fields.push_back(oil);

    BSONObj query = fromjson("{a: {$type: ['date', 'bool']}}");
    auto expr = parse(query);

    auto collCard = makeCard(100);
    ASSERT_EQ(estimateIndexBounds(bounds, collCard),
              estimateLeafMatchExpression(expr.get(), collCard));
}

TEST(HeuristicEstimate, BitsExpression) {
    BSONObj bitsAllClearQuery = fromjson("{a: {$bitsAllClear: [1, 5]}}");
    auto bitsAllClearExpr = parse(bitsAllClearQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsAllClearExpr.get(), makeCard(100)), kBitsSel);

    BSONObj bitsAllSetQuery = fromjson("{a: {$bitsAllSet: [1, 5]}}");
    auto bitsSetClearExpr = parse(bitsAllSetQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsSetClearExpr.get(), makeCard(100)), kBitsSel);

    BSONObj bitsAnyClearQuery = fromjson("{a: {$bitsAnyClear: [1, 5]}}");
    auto bitsAnyClearExpr = parse(bitsAnyClearQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsAnyClearExpr.get(), makeCard(100)), kBitsSel);

    BSONObj bitsAnySetQuery = fromjson("{a: {$bitsAnySet: [1, 5]}}");
    auto bitsAnySetExpr = parse(bitsAnySetQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsAnySetExpr.get(), makeCard(100)), kBitsSel);
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
