/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimation_types.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

class JoinCostEstimatorTest : public JoinOrderingTestFixture {
public:
    void setUp() override {
        JoinOrderingTestFixture::setUp();
        smallNss = NamespaceString::createNamespaceString_forTest("foo");
        largeNss = NamespaceString::createNamespaceString_forTest("bar");
        smallNodeId = *graph.addNode(smallNss, makeCanonicalQuery(smallNss), boost::none);
        largeNodeId = *graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);
        unselectiveNodeId = *graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);
        jCtx.emplace(makeContext());

        SubsetCardinalities subsetCards{
            {makeNodeSet(smallNodeId), makeCard(100)},
            {makeNodeSet(largeNodeId), makeCard(10'000)},
            {makeNodeSet(unselectiveNodeId), makeCard(100'000)},
        };
        NodeCardinalities collCards{makeCard(100'000), makeCard(200'000), makeCard(200'000)};

        cardEstimator =
            std::make_unique<FakeJoinCardinalityEstimator>(*jCtx, subsetCards, collCards);

        constexpr double docSizeBytes = 500;
        catalogStats = {.collStats = {
                            {smallNss,
                             CollectionStats{.allocatedDataPageBytes =
                                                 collCards[smallNodeId].toDouble() * docSizeBytes}},
                            {largeNss,
                             CollectionStats{.allocatedDataPageBytes =
                                                 collCards[largeNodeId].toDouble() * docSizeBytes}},
                        }};

        planEnumCtx =
            std::make_unique<PlanEnumeratorContext>(*jCtx, std::move(cardEstimator), false);
        costEstimator = std::make_unique<JoinCostEstimatorImpl>(
            *jCtx, *planEnumCtx->getJoinCardinalityEstimator(), catalogStats);
    }

    NamespaceString smallNss;
    NamespaceString largeNss;
    NodeId smallNodeId;
    NodeId largeNodeId;
    NodeId unselectiveNodeId;
    boost::optional<JoinReorderingContext> jCtx;
    std::unique_ptr<PlanEnumeratorContext> planEnumCtx;
    std::unique_ptr<JoinCardinalityEstimator> cardEstimator;
    CatalogStats catalogStats;
    std::unique_ptr<JoinCostEstimator> costEstimator;
};

TEST_F(JoinCostEstimatorTest, LargerCollectionHasHigherCost) {
    auto smallCost = costEstimator->costCollScanFragment(smallNodeId);
    auto largeCost = costEstimator->costCollScanFragment(largeNodeId);
    ASSERT_GT(largeCost, smallCost);
}

TEST_F(JoinCostEstimatorTest, LargerIndexScanHasHigherCost) {
    auto smallCost = costEstimator->costIndexScanFragment(smallNodeId);
    auto largeCost = costEstimator->costIndexScanFragment(largeNodeId);
    ASSERT_GT(largeCost, smallCost);
}

TEST_F(JoinCostEstimatorTest, SelectiveIndexScanHasSmallerCostThanCollScan) {
    auto collScanCost = costEstimator->costCollScanFragment(smallNodeId);
    auto indexScanCost = costEstimator->costIndexScanFragment(smallNodeId);
    ASSERT_GT(collScanCost, indexScanCost);
}

TEST_F(JoinCostEstimatorTest, UnselectiveIndexScanHasLargerCostThanCollScan) {
    auto collScanCost = costEstimator->costCollScanFragment(unselectiveNodeId);
    auto indexScanCost = costEstimator->costIndexScanFragment(unselectiveNodeId);
    ASSERT_GT(indexScanCost, collScanCost);
}

const JoinSubset& getJoinSubsetForNodeId(const std::vector<JoinSubset>& subsets, NodeId nodeId) {
    return *std::find_if(subsets.cbegin(), subsets.cend(), [&](const JoinSubset& subset) {
        return subset.getNodeId() == nodeId;
    });
}

const JoinCostEstimate zeroJoinCost(zeroCE, zeroCE, zeroCE, zeroCE);

// Verify that HJ(largeNss, foo) > HJ(smallNss, foo)
TEST_F(JoinCostEstimatorTest, HashJoinLargerBuildSideHasLargerCost) {
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};
    BaseNode unselectiveBaseNode{.nss = largeNss, .node = unselectiveNodeId, .cost = zeroJoinCost};
    auto smallHjCost = costEstimator->costHashJoinFragment(smallBaseNode, unselectiveBaseNode);
    auto largeHjCost = costEstimator->costHashJoinFragment(largeBaseNode, unselectiveBaseNode);
    ASSERT_GT(largeHjCost, smallHjCost);
}

TEST_F(JoinCostEstimatorTest, HashJoinChildCostTakenIntoAccount) {
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};
    BaseNode smallBaseNodeNonZero{.nss = smallNss,
                                  .node = smallNodeId,
                                  .cost = JoinCostEstimate(makeCard(1), zeroCE, zeroCE, zeroCE)};
    auto smallHjCost = costEstimator->costHashJoinFragment(smallBaseNode, largeBaseNode);
    auto largeHjCost = costEstimator->costHashJoinFragment(smallBaseNodeNonZero, largeBaseNode);
    ASSERT_GT(largeHjCost, smallHjCost);
}

}  // namespace mongo::join_ordering
