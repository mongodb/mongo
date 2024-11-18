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

#include "mongo/db/query/cost_based_ranker/cardinality_estimator.h"

#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stats/collection_statistics_impl.h"
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

IndexEntry buildSimpleIndexEntry(const std::vector<std::string>& indexFields) {
    BSONObjBuilder bob;
    for (auto& fieldName : indexFields) {
        bob.append(fieldName, 1);
    }
    BSONObj kp = bob.obj();

    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexDescriptor::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

std::unique_ptr<IndexScanNode> makeIndexScan(IndexBounds bounds,
                                             std::vector<std::string> indexFields,
                                             std::unique_ptr<MatchExpression> filter = nullptr) {
    auto indexScan = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(indexFields));
    indexScan->bounds = std::move(bounds);
    if (filter) {
        indexScan->filter = std::move(filter);
    }
    return indexScan;
}

std::unique_ptr<QuerySolution> makeIndexScanFetchPlan(
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::unique_ptr<MatchExpression> indexFilter = nullptr,
    std::unique_ptr<MatchExpression> fetchFilter = nullptr) {

    auto fetch =
        std::make_unique<FetchNode>(makeIndexScan(bounds, indexFields, std::move(indexFilter)));
    if (fetchFilter) {
        fetch->filter = std::move(fetchFilter);
    }

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(fetch));
    return solution;
}

std::unique_ptr<QuerySolution> makeCollScanPlan(std::unique_ptr<MatchExpression> filter) {
    auto scan = std::make_unique<CollectionScanNode>();
    if (filter) {
        scan->filter = std::move(filter);
    }

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(scan));
    return solution;
}

IndexBounds makePointIntervalBounds(double point, std::string fieldName) {
    OrderedIntervalList oil(fieldName);
    oil.intervals.emplace_back(IndexBoundsBuilder::makePointInterval(point));
    IndexBounds bounds;
    bounds.fields.emplace_back(std::move(oil));
    return bounds;
}

IndexBounds makeRangeIntervalBounds(const BSONObj& range,
                                    BoundInclusion boundInclusion,
                                    std::string fieldName) {
    OrderedIntervalList oilRange;
    oilRange.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(range, boundInclusion));
    IndexBounds rangeBounds;
    rangeBounds.fields.push_back(oilRange);
    return rangeBounds;
}

CardinalityEstimate getPlanCE(const QuerySolution& plan, double collCE) {
    EstimateMap qsnEstimates;
    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    stats::CollectionStatisticsImpl stats(collCE, kNss);
    CardinalityEstimator estimator{stats, qsnEstimates, QueryPlanRankerModeEnum::kHeuristicCE};
    estimator.estimatePlan(plan);
    return qsnEstimates.at(plan.root()).outCE;
}

TEST(CardinalityEstimator, PointInterval) {
    std::vector<std::string> indexFields = {"a"};
    auto plan = makeIndexScanFetchPlan(makePointIntervalBounds(5.0, indexFields[0]), indexFields);
    ASSERT_EQ(getPlanCE(*plan, 100.0), makeCard(10.0));
}

TEST(CardinalityEstimator, ManyPointIntervals) {
    std::vector<std::string> indexFields = {"a"};
    OrderedIntervalList oil(indexFields[0]);
    for (size_t i = 0; i < 5; ++i) {
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i));
    }
    IndexBounds bounds;
    bounds.fields.push_back(oil);
    auto plan = makeIndexScanFetchPlan(std::move(bounds), indexFields);
    ASSERT_EQ(getPlanCE(*plan, 100.0), makeCard(50.0));
}

TEST(CardinalityEstimator, CompoundIndex) {
    IndexBounds bounds;
    std::vector<std::string> indexFields = {"a", "b", "c", "d", "e"};
    for (size_t i = 0; i < indexFields.size(); ++i) {
        OrderedIntervalList oil(indexFields[i]);
        for (size_t j = 0; j < 7; ++j) {
            oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i * j));
        }
        bounds.fields.push_back(oil);
    }
    auto plan = makeIndexScanFetchPlan(std::move(bounds), indexFields);
    ASSERT_EQ(getPlanCE(*plan, 100.0), makeCard(51.2341));
}

TEST(CardinalityEstimator, PointMoreSelectiveThanRange) {
    std::vector<std::string> indexFields = {"a"};
    auto pointPlan =
        makeIndexScanFetchPlan(makePointIntervalBounds(5.0, indexFields[0]), indexFields);

    IndexBounds rangeBounds = makeRangeIntervalBounds(
        BSON("" << 5 << " " << 6), BoundInclusion::kIncludeBothStartAndEndKeys, indexFields[0]);
    auto rangePlan = makeIndexScanFetchPlan(std::move(rangeBounds), indexFields);

    ASSERT_LT(getPlanCE(*pointPlan, 100.0), getPlanCE(*rangePlan, 100.0));
}

TEST(CardinalityEstimator, CompoundBoundsMoreSelectiveThanSingleField) {
    std::vector<std::string> indexFields = {"a", "b"};
    OrderedIntervalList oil1(indexFields[0]);
    oil1.intervals.push_back(IndexBoundsBuilder::makePointInterval(5));

    IndexBounds singleField;
    singleField.fields.push_back(oil1);
    auto singleFieldPlan = makeIndexScanFetchPlan(std::move(singleField), indexFields);

    IndexBounds compoundBounds;
    compoundBounds.fields.push_back(oil1);
    OrderedIntervalList oil2 = oil1;
    oil2.name = indexFields[1];
    compoundBounds.fields.push_back(oil2);
    auto compoundBoundsPlan = makeIndexScanFetchPlan(std::move(compoundBounds), indexFields);

    ASSERT_LT(getPlanCE(*compoundBoundsPlan, 100.0), getPlanCE(*singleFieldPlan, 100.0));
}

TEST(CardinalityEstimator, PointIntervalSelectivityDependsOnInputCard) {
    std::vector<std::string> indexFields = {"a"};
    auto plan = makeIndexScanFetchPlan(makePointIntervalBounds(5.0, indexFields[0]), indexFields);

    ASSERT_LT(getPlanCE(*plan, 10000.0).toDouble() / 10000.0,
              getPlanCE(*plan, 100.0).toDouble() / 100.0);
}

TEST(CardinalityEstimator, EqualityMatchesIndexPointInterval) {
    std::vector<std::string> indexFields = {"a"};
    // Bounds for [5,5]
    auto indexPlan =
        makeIndexScanFetchPlan(makePointIntervalBounds(5.0, indexFields[0]), indexFields);

    // Expression for a = 5
    BSONObj query = fromjson("{a: 5}");
    auto expr = parse(query);
    auto collPlan = makeCollScanPlan(std::move(expr));

    auto collCard = 100;
    ASSERT_EQ(getPlanCE(*indexPlan, collCard), getPlanCE(*collPlan, collCard));
}

TEST(CardinalityEstimator, InequalityMatchesRangeOpenInterval) {
    std::vector<std::string> indexFields = {"a"};
    // Bounds for (5,inf]
    IndexBounds bounds =
        makeRangeIntervalBounds(BSON("" << 5.0 << " " << std::numeric_limits<double>::infinity()),
                                BoundInclusion::kIncludeEndKeyOnly,
                                indexFields[0]);
    auto indexPlan = makeIndexScanFetchPlan(bounds, indexFields);

    // Expression for a > 5
    BSONObj query = fromjson("{a: {$gt: 5}}");
    auto expr = parse(query);
    auto collPlan = makeCollScanPlan(std::move(expr));

    auto collCard = 100;
    ASSERT_EQ(getPlanCE(*indexPlan, collCard), getPlanCE(*collPlan, collCard));
}

TEST(CardinalityEstimator, InequalityMatchesRangeClosedInterval) {
    std::vector<std::string> indexFields = {"a"};
    // Bounds for [5,inf]
    IndexBounds bounds =
        makeRangeIntervalBounds(BSON("" << 5 << " " << std::numeric_limits<double>::infinity()),
                                BoundInclusion::kIncludeBothStartAndEndKeys,
                                indexFields[0]);
    auto indexPlan = makeIndexScanFetchPlan(bounds, indexFields);

    // Expression for a >= 5
    BSONObj query = fromjson("{a: {$gte: 5}}");
    auto expr = parse(query);
    auto collPlan = makeCollScanPlan(std::move(expr));

    auto collCard = 100;
    ASSERT_EQ(getPlanCE(*indexPlan, collCard), getPlanCE(*collPlan, collCard));
}

TEST(CardinalityEstimator, InExpressionMatchesIntervals) {
    std::vector<std::string> indexFields = {"a"};
    // Interval for [[1,1], [2,2], [3,3]]
    OrderedIntervalList oil;
    for (size_t i = 0; i < 3; ++i) {
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i));
    }
    IndexBounds bounds;
    bounds.fields.push_back(oil);
    auto indexPlan = makeIndexScanFetchPlan(bounds, indexFields);

    BSONObj query = fromjson("{a: {$in: [1,2,3]}}");
    auto expr = parse(query);
    auto collPlan = makeCollScanPlan(std::move(expr));

    auto collCard = 100;
    ASSERT_EQ(getPlanCE(*indexPlan, collCard), getPlanCE(*collPlan, collCard));
}

TEST(CardinalityEstimator, TypeExpressionMatchesIntervals) {
    std::vector<std::string> indexFields = {"a"};
    OrderedIntervalList oil(indexFields[0]);
    oil.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << Date_t::min() << " " << Date_t::max()),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << false << " " << true), BoundInclusion::kIncludeBothStartAndEndKeys));
    IndexBounds bounds;
    bounds.fields.push_back(oil);
    auto indexPlan = makeIndexScanFetchPlan(bounds, indexFields);

    BSONObj query = fromjson("{a: {$type: ['date', 'bool']}}");
    auto expr = parse(query);
    auto collPlan = makeCollScanPlan(std::move(expr));

    auto collCard = 100;
    ASSERT_EQ(getPlanCE(*indexPlan, collCard), getPlanCE(*collPlan, collCard));
}

TEST(CardinalityEstimator, ThreeOrsWithImplicitAnd) {
    std::vector<std::string> indexFields = {"a", "b", "c"};
    // Interval for [[1,1], [2,2]]
    OrderedIntervalList oil(indexFields[0]);
    for (size_t i = 0; i < 2; ++i) {
        oil.intervals.push_back(IndexBoundsBuilder::makePointInterval(i));
    }
    IndexBounds bounds;
    bounds.fields.push_back(oil);

    BSONObj indexCond = fromjson("{$or: [{b: /abc/}, {c: /def/}]}");
    auto indexExpr = parse(indexCond);

    BSONObj fetchCond = fromjson("{$or: [{x: 'abc'}, {y: 42}]}");
    auto fetchExpr = parse(indexCond);

    auto indexPlan =
        makeIndexScanFetchPlan(bounds, indexFields, std::move(indexExpr), std::move(fetchExpr));
    ASSERT_EQ(getPlanCE(*indexPlan, 1000), makeCard(14.9523));
}

TEST(CardinalityEstimator, ThreeOrsWithAndChildrenImplicitAnd) {
    std::vector<std::string> indexFields = {"a", "b", "c"};
    IndexBounds bounds = makePointIntervalBounds(13.0, indexFields[0]);
    constexpr const char* orWithAndChildren =
        "{$or: [{b: /abc/},"
        "       {$and: [{b: {$gt: 5}}, {b: {$gt: 7}}]},"
        "       {$and: [{c: {$eq: 6}}, {c: {$eq: 9}}]}"
        "]}";

    BSONObj indexCond = fromjson(orWithAndChildren);
    auto indexExpr = parse(indexCond);
    auto fetchExpr = indexExpr->clone();

    auto indexPlan =
        makeIndexScanFetchPlan(bounds, indexFields, std::move(indexExpr), std::move(fetchExpr));
    ASSERT_EQ(getPlanCE(*indexPlan, 1000), makeCard(10.5793));
}

TEST(CardinalityEstimator, IndexIntersectionWithFetchFilter) {
    std::vector<std::string> indexFields1 = {"a"};
    // First index scan
    IndexBounds rangeBounds = makeRangeIntervalBounds(
        BSON("" << 5 << " " << 6), BoundInclusion::kIncludeBothStartAndEndKeys, indexFields1[0]);
    auto indexScan1 = makeIndexScan(rangeBounds, indexFields1);

    // Second index scan
    std::vector<std::string> indexFields2 = {"a", "b", "c"};
    constexpr const char* orWithAndChildren =
        "{$or: [{b: /abc/},"
        "       {$and: [{b: {$gt: 5}}, {b: {$gt: 7}}]},"
        "       {$and: [{c: {$eq: 6}}, {c: {$eq: 9}}]}"
        "]}";

    BSONObj indexCond2 = fromjson(orWithAndChildren);
    auto indexExpr2 = parse(indexCond2);
    auto indexScan2 = makeIndexScan(
        makePointIntervalBounds(13.0, indexFields2[0]), indexFields2, std::move(indexExpr2));

    // Index intersection 1
    auto andHashNode1 = std::make_unique<AndHashNode>();
    andHashNode1->children.push_back(indexScan1->clone());
    andHashNode1->children.push_back(indexScan2->clone());

    // Index intersection 2 - child scans are in reverse order
    auto andHashNode2 = std::make_unique<AndHashNode>();
    andHashNode2->children.push_back(std::move(indexScan2));
    andHashNode2->children.push_back(std::move(indexScan1));

    // Make two complete intersection plans that only differ in the order of child index scans
    BSONObj fetchCond = fromjson("{$or: [{x: 'abc'}, {y: 42}]}");
    auto fetchExpr = parse(fetchCond);
    auto fetch1 = std::make_unique<FetchNode>(std::move(andHashNode1));
    auto fetch2 = std::make_unique<FetchNode>(std::move(andHashNode2));
    fetch1->filter = fetchExpr->clone();
    fetch2->filter = std::move(fetchExpr);
    auto intersectionPlan1 = std::make_unique<QuerySolution>();
    auto intersectionPlan2 = std::make_unique<QuerySolution>();
    intersectionPlan1->setRoot(std::move(fetch1));
    intersectionPlan2->setRoot(std::move(fetch2));

    CardinalityEstimate e1 = getPlanCE(*intersectionPlan1, 1000);
    CardinalityEstimate e2 = getPlanCE(*intersectionPlan2, 1000);

    ASSERT_EQ(e1, e2);
    ASSERT_EQ(e1, makeCard(3.8222));
}

TEST(CardinalityEstimator, IndexUnionWithFetchFilter) {
    std::vector<std::string> indexFields1 = {"a"};
    // First index scan
    IndexBounds rangeBounds = makeRangeIntervalBounds(
        BSON("" << 5 << " " << 6), BoundInclusion::kIncludeBothStartAndEndKeys, indexFields1[0]);
    auto indexScan1 = makeIndexScan(rangeBounds, indexFields1);

    // Second index scan
    std::vector<std::string> indexFields2 = {"a", "b", "c"};
    constexpr const char* andWithAndChildren =
        "{$and: [{b: /abc/},"
        "       {$and: [{b: {$gt: 5}}, {b: {$gt: 7}}]},"
        "       {$and: [{c: {$eq: 6}}, {c: {$eq: 9}}]}"
        "]}";

    BSONObj indexCond2 = fromjson(andWithAndChildren);
    auto indexExpr2 = parse(indexCond2);
    auto indexScan2 = makeIndexScan(
        makePointIntervalBounds(13.0, indexFields2[0]), indexFields2, std::move(indexExpr2));

    // Index union 1
    auto orNode1 = std::make_unique<OrNode>();
    orNode1->children.push_back(indexScan1->clone());
    orNode1->children.push_back(indexScan2->clone());

    // Index union 2 - child scans are in reverse order
    auto orNode2 = std::make_unique<OrNode>();
    orNode2->children.push_back(std::move(indexScan2));
    orNode2->children.push_back(std::move(indexScan1));

    // Make two complete union plans that only differ in the order of child index scans
    BSONObj fetchCond = fromjson("{$or: [{x: 'abc'}, {y: 42}]}");
    auto fetchExpr = parse(fetchCond);
    auto fetch1 = std::make_unique<FetchNode>(std::move(orNode1));
    auto fetch2 = std::make_unique<FetchNode>(std::move(orNode2));
    fetch1->filter = fetchExpr->clone();
    fetch2->filter = std::move(fetchExpr);
    auto unionPlan1 = std::make_unique<QuerySolution>();
    auto unionPlan2 = std::make_unique<QuerySolution>();
    unionPlan1->setRoot(std::move(fetch1));
    unionPlan2->setRoot(std::move(fetch2));

    CardinalityEstimate e1 = getPlanCE(*unionPlan1, 1000);
    CardinalityEstimate e2 = getPlanCE(*unionPlan2, 1000);

    ASSERT_EQ(e1, e2);
    ASSERT_EQ(e1, makeCard(20.8395));
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
