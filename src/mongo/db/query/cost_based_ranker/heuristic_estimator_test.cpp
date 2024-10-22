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

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
