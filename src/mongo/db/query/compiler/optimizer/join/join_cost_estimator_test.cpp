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
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
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
        extremelySmallNss = NamespaceString::createNamespaceString_forTest("baz");
        smallNodeId = *graph.addNode(smallNss, makeCanonicalQuery(smallNss), boost::none);
        largeNodeId = *graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);
        unselectiveNodeId = *graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);
        extremelySmallNodeId =
            *graph.addNode(extremelySmallNss, makeCanonicalQuery(extremelySmallNss), boost::none);
        jCtx.emplace(makeContext());

        SubsetCardinalities subsetCards{
            {makeNodeSet(smallNodeId), makeCard(10)},
            {makeNodeSet(largeNodeId), makeCard(1'000'000)},
            {makeNodeSet(unselectiveNodeId), makeCard(2'000'000)},
            {makeNodeSet(extremelySmallNodeId), makeCard(1)},
            {makeNodeSet(smallNodeId, largeNodeId), makeCard(1)},
            {makeNodeSet(smallNodeId, unselectiveNodeId), makeCard(100'000)},
        };
        NodeCardinalities collCards{
            makeCard(100'000), makeCard(2'000'000), makeCard(2'000'000), makeCard(1)};

        auto cardEstimator =
            std::make_unique<FakeJoinCardinalityEstimator>(*jCtx, subsetCards, collCards);

        constexpr double docSizeBytes = 500;
        jCtx->catStats = {
            .collStats = {
                {smallNss,
                 CollectionStats{.allocatedDataPageBytes =
                                     collCards[smallNodeId].toDouble() * docSizeBytes}},
                {largeNss,
                 CollectionStats{.allocatedDataPageBytes =
                                     collCards[largeNodeId].toDouble() * docSizeBytes}},
                {extremelySmallNss,
                 CollectionStats{.allocatedDataPageBytes =
                                     collCards[extremelySmallNodeId].toDouble() * docSizeBytes}},
            }};

        auto costEstimator = std::make_unique<JoinCostEstimatorImpl>(*jCtx, *cardEstimator);
        planEnumCtx = std::make_unique<PlanEnumeratorContext>(
            *jCtx,
            std::move(cardEstimator),
            std::move(costEstimator),
            EnumerationStrategy{.planShape = PlanTreeShape::ZIG_ZAG,
                                .mode = PlanEnumerationMode::CHEAPEST,
                                .enableHJOrderPruning = true});
    }

    NamespaceString smallNss;
    NamespaceString largeNss;
    NamespaceString extremelySmallNss;
    NodeId smallNodeId;
    NodeId largeNodeId;
    NodeId unselectiveNodeId;
    NodeId extremelySmallNodeId;
    boost::optional<JoinReorderingContext> jCtx;
    std::unique_ptr<PlanEnumeratorContext> planEnumCtx;
    CatalogStats catalogStats;
    JoinCostEstimate zeroJoinCost =
        JoinCostEstimate(CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code},
                         CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code},
                         CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code},
                         CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code});
};

TEST_F(JoinCostEstimatorTest, LargerCollectionHasHigherCost) {
    auto smallCost = planEnumCtx->getJoinCostEstimator()->costCollScanFragment(smallNodeId);
    auto largeCost = planEnumCtx->getJoinCostEstimator()->costCollScanFragment(largeNodeId);
    ASSERT_GT(largeCost, smallCost);
}

TEST_F(JoinCostEstimatorTest, LargerIndexScanHasHigherCost) {
    auto smallCost = planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(smallNodeId);
    auto largeCost = planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(largeNodeId);
    ASSERT_GT(largeCost, smallCost);
}

TEST_F(JoinCostEstimatorTest, SelectiveIndexScanHasSmallerCostThanCollScan) {
    auto collScanCost = planEnumCtx->getJoinCostEstimator()->costCollScanFragment(smallNodeId);
    auto indexScanCost = planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(smallNodeId);
    ASSERT_GT(collScanCost, indexScanCost);
}

TEST_F(JoinCostEstimatorTest, UnselectiveIndexScanHasLargerCostThanCollScan) {
    auto collScanCost =
        planEnumCtx->getJoinCostEstimator()->costCollScanFragment(unselectiveNodeId);
    auto indexScanCost =
        planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(unselectiveNodeId);
    ASSERT_GT(indexScanCost, collScanCost);
}

const JoinSubset& getJoinSubsetForNodeId(const std::vector<JoinSubset>& subsets, NodeId nodeId) {
    return *std::find_if(subsets.cbegin(), subsets.cend(), [&](const JoinSubset& subset) {
        return subset.getNodeId() == nodeId;
    });
}

// Verify that HJ(largeNss, foo) > HJ(smallNss, foo)
TEST_F(JoinCostEstimatorTest, HashJoinLargerBuildSideHasLargerCost) {
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};
    BaseNode unselectiveBaseNode{.nss = largeNss, .node = unselectiveNodeId, .cost = zeroJoinCost};
    auto smallHjCost = planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(
        smallBaseNode, unselectiveBaseNode);
    auto largeHjCost = planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(
        largeBaseNode, unselectiveBaseNode);
    ASSERT_GT(largeHjCost, smallHjCost);
}

TEST_F(JoinCostEstimatorTest, HashJoinChildCostTakenIntoAccount) {
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};
    BaseNode smallBaseNodeNonZero{.nss = smallNss,
                                  .node = smallNodeId,
                                  .cost =
                                      JoinCostEstimate(makeCard(10), zeroCE, makeCard(10), zeroCE)};
    auto smallHjCost =
        planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(smallBaseNode, largeBaseNode);
    auto largeHjCost = planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(
        smallBaseNodeNonZero, largeBaseNode);
    ASSERT_GT(largeHjCost, smallHjCost);
}

TEST_F(JoinCostEstimatorTest, INLJLargerLeftSideHasLargerCost) {
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};
    auto smallINLJCost = planEnumCtx->getJoinCostEstimator()->costINLJFragment(
        smallBaseNode, unselectiveNodeId, nullptr);
    auto largeINLJCost = planEnumCtx->getJoinCostEstimator()->costINLJFragment(
        largeBaseNode, unselectiveNodeId, nullptr);
    ASSERT_GT(largeINLJCost, smallINLJCost);
}

TEST_F(JoinCostEstimatorTest, INLJLowerCostThanHashJoin) {
    // When the left side is small and the index is selective, INLJ should be cheaper than hash join
    // (little random IO).
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};

    auto inljCost =
        planEnumCtx->getJoinCostEstimator()->costINLJFragment(smallBaseNode, largeNodeId, nullptr);
    auto hjCost =
        planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(smallBaseNode, largeBaseNode);

    ASSERT_LT(inljCost, hjCost);
}

TEST_F(JoinCostEstimatorTest, INLJHigherCostThanHashJoin) {
    // When the left side is large and the index is unselective, INLJ should be more expensive than
    // hash join (lots of random IO).
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode unselectiveBaseNode{.nss = largeNss, .node = unselectiveNodeId, .cost = zeroJoinCost};

    auto inljCost = planEnumCtx->getJoinCostEstimator()->costINLJFragment(
        smallBaseNode, unselectiveNodeId, nullptr);
    auto hjCost = planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(smallBaseNode,
                                                                            unselectiveBaseNode);

    ASSERT_GT(inljCost, hjCost);
}

TEST_F(JoinCostEstimatorTest, NLJHigherCostThanHashJoin) {
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};
    auto nljCost =
        planEnumCtx->getJoinCostEstimator()->costNLJFragment(smallBaseNode, largeBaseNode);
    auto hjCost =
        planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(smallBaseNode, largeBaseNode);
    ASSERT_GT(nljCost, hjCost);
}

TEST_F(JoinCostEstimatorTest, NLJLowerCostThanHashJoin) {
    // NLJ is better than HashJoin in the case where both collections are extremely small and the
    // overhead of building the hash table is more than performing the additional IO.
    BaseNode extremelySmallNode{
        .nss = extremelySmallNss, .node = extremelySmallNodeId, .cost = zeroJoinCost};
    auto nljCost = planEnumCtx->getJoinCostEstimator()->costNLJFragment(extremelySmallNode,
                                                                        extremelySmallNode);
    auto hjCost = planEnumCtx->getJoinCostEstimator()->costHashJoinFragment(extremelySmallNode,
                                                                            extremelySmallNode);
    ASSERT_LT(nljCost, hjCost);
}

}  // namespace mongo::join_ordering
