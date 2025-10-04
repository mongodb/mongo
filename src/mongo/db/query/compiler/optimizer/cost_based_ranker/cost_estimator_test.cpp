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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cost_estimator.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/unittest/unittest.h"

namespace mongo::cost_based_ranker {
namespace {

TEST(CostEstimator, FullCollScanVsFilteredCollScan) {
    EstimateMap estimates;

    auto fullCollScan = makeCollScanPlan(nullptr);
    estimates[fullCollScan->root()] = QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(100)};

    BSONObj query = fromjson("{a: {$gt: 5}}");
    auto collScanFilter = makeCollScanPlan(parse(query));
    // The predicate filters out 50 documents.
    estimates[collScanFilter->root()] = QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(50)};

    CostEstimator costEstimator{estimates};
    costEstimator.estimatePlan(*fullCollScan);
    costEstimator.estimatePlan(*collScanFilter);

    ASSERT_LT(estimates[fullCollScan->root()].cost, estimates[collScanFilter->root()].cost);
}

TEST(CostEstimator, FilterCost) {
    // Test that a more complex filter with the same selectivity results in a higher cost scan node.
    const QSNEstimate cardEst{.inCE = makeCard(100), .outCE = makeCard(50)};
    EstimateMap estimates;

    BSONObj simplePred = fromjson("{a: {$gt: 5}}");
    auto cheapPlan = makeCollScanPlan(parse(simplePred));
    estimates[cheapPlan->root()] = cardEst;

    BSONObj complexPred =
        fromjson("{a: {$gt: 5, $lt: 10}, b: {$elemMatch: {$gt: 'abc', $lt: 'def'}}}");
    auto expensivePlan = makeCollScanPlan(parse(complexPred));
    estimates[expensivePlan->root()] = cardEst;

    CostEstimator costEstimator{estimates};
    costEstimator.estimatePlan(*cheapPlan);
    costEstimator.estimatePlan(*expensivePlan);

    ASSERT_LT(estimates[cheapPlan->root()].cost, estimates[expensivePlan->root()].cost);
}

TEST(CostEstimator, VirtualScan) {
    EstimateMap estimates;

    auto fullCollScan = makeVirtualCollScanPlan(100, nullptr);
    estimates[fullCollScan->root()] = QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(100)};

    BSONObj query = fromjson("{a: {$gt: 5}}");
    auto collScanFilter = makeVirtualCollScanPlan(100, parse(query));
    // The predicate filters out 50 documents.
    estimates[collScanFilter->root()] = QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(50)};

    CostEstimator costEstimator{estimates};
    costEstimator.estimatePlan(*fullCollScan);
    costEstimator.estimatePlan(*collScanFilter);

    ASSERT_LT(estimates[fullCollScan->root()].cost, estimates[collScanFilter->root()].cost);
}

TEST(CostEstimator, PointIndexScanLessCostThanRange) {
    EstimateMap estimates;

    auto pointIndexScan = makeIndexScanFetchPlan(makePointIntervalBounds(1, "a"), {"a"});
    // Fetch
    estimates[pointIndexScan->root()] = QSNEstimate{.outCE = makeCard(1)};
    // IndexScan
    estimates[pointIndexScan->root()->children[0].get()] =
        QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(1)};

    auto rangeIndexScan = makeIndexScanFetchPlan(
        makeRangeIntervalBounds(
            BSON("" << 5 << "" << 6), BoundInclusion::kIncludeBothStartAndEndKeys, "a"),
        {"a"});
    // Fetch
    estimates[rangeIndexScan->root()] = QSNEstimate{.outCE = makeCard(10)};
    // IndexScan
    estimates[rangeIndexScan->root()->children[0].get()] =
        QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(10)};

    CostEstimator costEstimator{estimates};
    costEstimator.estimatePlan(*pointIndexScan);
    costEstimator.estimatePlan(*rangeIndexScan);

    // Cost of point scan plan should be less than that of the range scan
    ASSERT_LT(estimates[pointIndexScan->root()].cost, estimates[rangeIndexScan->root()].cost);
    // Cost of fetch node should be greater than cost of the index scan as costs are cumulative
    ASSERT_GT(estimates[pointIndexScan->root()].cost,
              estimates[pointIndexScan->root()->children[0].get()].cost);
}

std::unique_ptr<IndexScanNode> indexScanNode(EstimateMap& estimates, QSNEstimate est) {
    auto node = makeIndexScan(makePointIntervalBounds(1, "a"), {"a"});
    estimates[node.get()] = est;
    return node;
}

template <typename IndexCombinationNode>
void testIndexCombinationDependsOnChildren() {
    EstimateMap estimates;
    auto indexIntersectNode = std::make_unique<IndexCombinationNode>();
    indexIntersectNode->addChildren([&]() {
        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.push_back(
            indexScanNode(estimates, QSNEstimate{.inCE = makeCard(10), .outCE = makeCard(10)}));
        children.push_back(
            indexScanNode(estimates, QSNEstimate{.inCE = makeCard(10), .outCE = makeCard(10)}));
        return children;
    }());
    estimates[indexIntersectNode.get()] = QSNEstimate{.outCE = makeCard(5)};
    auto cheapPlan = std::make_unique<QuerySolution>();
    cheapPlan->setRoot(std::move(indexIntersectNode));

    auto expensiveIndexIntersectNode = std::make_unique<IndexCombinationNode>();
    expensiveIndexIntersectNode->addChildren([&]() {
        std::vector<std::unique_ptr<QuerySolutionNode>> children;
        children.push_back(
            indexScanNode(estimates, QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(100)}));
        children.push_back(
            indexScanNode(estimates, QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(100)}));
        return children;
    }());
    estimates[expensiveIndexIntersectNode.get()] = QSNEstimate{.outCE = makeCard(5)};
    auto expensivePlan = std::make_unique<QuerySolution>();
    expensivePlan->setRoot(std::move(expensiveIndexIntersectNode));

    CostEstimator costEstimator{estimates};
    costEstimator.estimatePlan(*cheapPlan);
    costEstimator.estimatePlan(*expensivePlan);
    ASSERT_LT(estimates[cheapPlan->root()].cost, estimates[expensivePlan->root()].cost);
}

// Increasing child cost increases the cost of index intersection and union plans
TEST(CostEstimator, IndexCombinationDependsOnChildren) {
    testIndexCombinationDependsOnChildren<AndHashNode>();
    testIndexCombinationDependsOnChildren<AndSortedNode>();
    testIndexCombinationDependsOnChildren<OrNode>();
    testIndexCombinationDependsOnChildren<MergeSortNode>();
}

std::unique_ptr<CollectionScanNode> collScanNode(EstimateMap& estimates, QSNEstimate est) {
    auto node = std::make_unique<CollectionScanNode>();
    estimates[node.get()] = est;
    return node;
}

template <typename SortNode>
void testSortCostDependsOnChildren() {
    EstimateMap estimates;
    auto cheapCollScan =
        collScanNode(estimates, QSNEstimate{.inCE = makeCard(10), .outCE = makeCard(10)});
    auto cheapSort = std::make_unique<SortNode>(
        std::move(cheapCollScan), BSON("a" << 1), 0, LimitSkipParameterization::Disabled);
    estimates[cheapSort.get()] = QSNEstimate{.outCE = makeCard(10)};
    auto cheapPlan = std::make_unique<QuerySolution>();
    cheapPlan->setRoot(std::move(cheapSort));

    auto expsensiveCollScan =
        collScanNode(estimates, QSNEstimate{.inCE = makeCard(100), .outCE = makeCard(100)});
    auto expensiveSort = std::make_unique<SortNode>(
        std::move(expsensiveCollScan), BSON("a" << 1), 0, LimitSkipParameterization::Disabled);
    estimates[expensiveSort.get()] = QSNEstimate{.outCE = makeCard(100)};
    auto expensivePlan = std::make_unique<QuerySolution>();
    expensivePlan->setRoot(std::move(expensiveSort));

    CostEstimator costEstimator{estimates};
    costEstimator.estimatePlan(*cheapPlan);
    costEstimator.estimatePlan(*expensivePlan);
    ASSERT_LT(estimates[cheapPlan->root()].cost, estimates[expensivePlan->root()].cost);
}

TEST(CostEstimator, SortDefaultOrSimple) {
    testSortCostDependsOnChildren<SortNodeDefault>();
    testSortCostDependsOnChildren<SortNodeSimple>();
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
