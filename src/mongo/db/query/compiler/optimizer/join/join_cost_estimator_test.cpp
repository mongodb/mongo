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
        selectiveNodeId =
            *graph.addNode(largeNss, makeCanonicalQuery(extremelySmallNss), boost::none);
        pointRightNodeId = *graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);
        rangeRightNodeId = *graph.addNode(largeNss, makeCanonicalQuery(largeNss), boost::none);

        graph.addEdge(smallNodeId, unselectiveNodeId, {});
        graph.addEdge(largeNodeId, unselectiveNodeId, {});
        graph.addEdge(smallNodeId, largeNodeId, {});
        graph.addEdge(smallNodeId, pointRightNodeId, {});
        graph.addEdge(smallNodeId, rangeRightNodeId, {});

        collCards = {
            makeCard(1000),         // smallNode
            makeCard(20'000),       // largeNode
            makeCard(20'000),       // unselectiveNode
            makeCard(1),            // extremelySmallNode
            makeCard(100'000'000),  // selectiveNode
            makeCard(20'000),       // pointRightNode
            makeCard(20'000),       // rangeRightNode
        };

        constexpr double docSizeBytes = 500;
        catStats = {
            .collStats = {
                {smallNss,
                 CollectionStats{collCards[smallNodeId].toDouble() * docSizeBytes,
                                 collCards[smallNodeId].toDouble() * docSizeBytes}},
                {largeNss,
                 CollectionStats{collCards[largeNodeId].toDouble() * docSizeBytes,
                                 collCards[largeNodeId].toDouble() * docSizeBytes}},
                {extremelySmallNss,
                 CollectionStats{collCards[extremelySmallNodeId].toDouble() * docSizeBytes,
                                 collCards[extremelySmallNodeId].toDouble() * docSizeBytes}},
            }};

        jCtx.emplace(makeContext());

        SubsetCardinalities subsetCards{
            {makeNodeSet(smallNodeId), makeCard(100)},
            {makeNodeSet(largeNodeId), makeCard(10'000)},
            {makeNodeSet(unselectiveNodeId), makeCard(20'000)},
            {makeNodeSet(extremelySmallNodeId), makeCard(1)},
            {makeNodeSet(selectiveNodeId), makeCard(1)},
            {makeNodeSet(pointRightNodeId), makeCard(20'000)},
            {makeNodeSet(rangeRightNodeId), makeCard(20'000)},
            {makeNodeSet(smallNodeId, largeNodeId), makeCard(10)},
            {makeNodeSet(smallNodeId, unselectiveNodeId), makeCard(1000)},
            {makeNodeSet(largeNodeId, unselectiveNodeId), makeCard(1000)},
            {makeNodeSet(smallNodeId, pointRightNodeId), makeCard(100)},
            {makeNodeSet(smallNodeId, rangeRightNodeId), makeCard(100)},
        };
        EdgeSelectivities edgeSel{
            makeSel(1000.0 / (1'000 * 20'000)),   // smallNode <--> unselectiveNode
            makeSel(1000.0 / (20'000 * 20'000)),  // largeNode <--> unselectiveNode
            makeSel(1.0 / (1000 * 20'000)),       // smallNode <--> largeId
            makeSel(1.0 / 20'000),                // smallNode <--> pointRightNode: 1 doc/probe
            makeSel(100.0 / 20'000),              // smallNode <--> rangeRightNode: 100 docs/probe
        };
        cardEstimator = std::make_unique<FakeJoinCardinalityEstimator>(*jCtx, subsetCards, edgeSel);

        costEstimator = std::make_unique<JoinCostEstimatorImpl>(*jCtx, *cardEstimator);
        planEnumCtx = std::make_unique<PlanEnumeratorContext>(
            *jCtx,
            cardEstimator.get(),
            costEstimator.get(),
            EnumerationStrategy{.planShape = PlanTreeShape::ZIG_ZAG,
                                .mode = PlanEnumerationMode::CHEAPEST,
                                .enableHJOrderPruning = true});
    }

    EdgeId getEdge(NodeId left, NodeId right) const {
        const auto& edges = jCtx->joinGraph.getJoinEdges(makeNodeSet(left), makeNodeSet(right));
        tassert(12062101, "expected exactly 1 edge", edges.size() == 1);
        return edges[0];
    }

    NamespaceString smallNss;
    NamespaceString largeNss;
    NamespaceString extremelySmallNss;
    NodeId smallNodeId;
    NodeId largeNodeId;
    NodeId unselectiveNodeId;
    NodeId extremelySmallNodeId;
    NodeId selectiveNodeId;
    NodeId pointRightNodeId;
    NodeId rangeRightNodeId;
    boost::optional<JoinReorderingContext> jCtx;
    std::unique_ptr<PlanEnumeratorContext> planEnumCtx;
    std::unique_ptr<JoinCostEstimator> costEstimator;
    std::unique_ptr<JoinCardinalityEstimator> cardEstimator;
    CatalogStats catalogStats;
    JoinCostEstimate zeroJoinCost =
        JoinCostEstimate(CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code},
                         CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code},
                         CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code},
                         CardinalityEstimate{CardinalityType{0.0}, EstimationSource::Code});
};

TEST_F(JoinCostEstimatorTest, LargerCollectionHasHigherCost) {
    auto smallCost =
        planEnumCtx->getJoinCostEstimator()->costCollScanFragment(smallNodeId, zeroCost);
    auto largeCost =
        planEnumCtx->getJoinCostEstimator()->costCollScanFragment(largeNodeId, zeroCost);
    ASSERT_GT(largeCost, smallCost);
}

TEST_F(JoinCostEstimatorTest, LargerIndexScanHasHigherCost) {
    auto smallCost =
        planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(smallNodeId, zeroCost);
    auto largeCost =
        planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(largeNodeId, zeroCost);
    ASSERT_GT(largeCost, smallCost);
}

TEST_F(JoinCostEstimatorTest, SelectiveIndexScanHasSmallerCostThanCollScan) {
    auto collScanCost =
        planEnumCtx->getJoinCostEstimator()->costCollScanFragment(selectiveNodeId, zeroCost);
    auto indexScanCost =
        planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(selectiveNodeId, zeroCost);
    ASSERT_GT(collScanCost, indexScanCost);
}

TEST_F(JoinCostEstimatorTest, UnselectiveIndexScanHasLargerCostThanCollScan) {
    auto collScanCost =
        planEnumCtx->getJoinCostEstimator()->costCollScanFragment(unselectiveNodeId, zeroCost);
    auto indexScanCost =
        planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(unselectiveNodeId, zeroCost);
    ASSERT_GT(indexScanCost, collScanCost);
}

TEST_F(JoinCostEstimatorTest, CBRCostAddedDirectlyToCollScanCost) {
    auto cbrCost = CostEstimate{CostType{5.0}, EstimationSource::Code};
    auto withoutCBR =
        planEnumCtx->getJoinCostEstimator()->costCollScanFragment(smallNodeId, zeroCost);
    auto withCBR = planEnumCtx->getJoinCostEstimator()->costCollScanFragment(smallNodeId, cbrCost);
    ASSERT_EQ(withCBR.getTotalCost(), withoutCBR.getTotalCost() + cbrCost);
}

TEST_F(JoinCostEstimatorTest, CBRCostAddedDirectlyToIndexScanCost) {
    auto cbrCost = CostEstimate{CostType{10.0}, EstimationSource::Code};
    auto withoutCBR =
        planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(smallNodeId, zeroCost);
    auto withCBR = planEnumCtx->getJoinCostEstimator()->costIndexScanFragment(smallNodeId, cbrCost);
    ASSERT_EQ(withCBR.getTotalCost(), withoutCBR.getTotalCost() + cbrCost);
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
        smallBaseNode, unselectiveNodeId, nullptr, getEdge(smallNodeId, unselectiveNodeId));
    auto largeINLJCost = planEnumCtx->getJoinCostEstimator()->costINLJFragment(
        largeBaseNode, unselectiveNodeId, nullptr, getEdge(largeNodeId, unselectiveNodeId));
    ASSERT_GT(largeINLJCost, smallINLJCost);
}

TEST_F(JoinCostEstimatorTest, INLJLowerCostThanHashJoin) {
    // When the left side is small and the index is selective, INLJ should be cheaper than hash join
    // (little random IO).
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    BaseNode largeBaseNode{.nss = largeNss, .node = largeNodeId, .cost = zeroJoinCost};

    auto inljCost = planEnumCtx->getJoinCostEstimator()->costINLJFragment(
        smallBaseNode, largeNodeId, nullptr, getEdge(smallNodeId, largeNodeId));
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
        smallBaseNode, unselectiveNodeId, nullptr, getEdge(smallNodeId, unselectiveNodeId));
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

TEST_F(JoinCostEstimatorTest, PointINLJCheaperThanRangeINLJ) {
    // A point index probe returns 1 doc per probe (selectivity = 1/collCard), while a range probe
    // returns 100 docs per probe (selectivity = 100/collCard). With the same left side driving the
    // same number of probes, the range scan incurs far more sorted-sparse I/O within each probe,
    // making it more expensive.
    BaseNode smallBaseNode{.nss = smallNss, .node = smallNodeId, .cost = zeroJoinCost};
    auto pointCost = costEstimator->costINLJFragment(
        smallBaseNode, pointRightNodeId, nullptr, getEdge(smallNodeId, pointRightNodeId));
    auto rangeCost = costEstimator->costINLJFragment(
        smallBaseNode, rangeRightNodeId, nullptr, getEdge(smallNodeId, rangeRightNodeId));
    ASSERT_LT(pointCost, rangeCost);
}

class IndexScanNDVCostTest : public JoinOrderingTestFixture {
public:
    void setUp() override {
        JoinOrderingTestFixture::setUp();

        nss = NamespaceString::createNamespaceString_forTest("test", "ndvTestColl");
        constexpr double collCardValue = 1000.0;
        constexpr double numDocsOutputValue = 200.0;
        constexpr double docSizeBytes = 500.0;

        auto cq = makeCanonicalQuery(nss);
        nodeId = *graph.addNode(nss, std::move(cq), boost::none);

        // Register an index scan QSN so that costIndexScanFragment can reach the NDV path.
        auto* cqPtr = graph.getNode(nodeId).accessPath.get();
        cbrCqQsns.emplace(cqPtr, makeIndexScanFetchPlan(nss, IndexBounds{}, {"a"}));

        collCards = {makeCard(collCardValue)};
        catStats = {.collStats = {{nss,
                                   CollectionStats{collCardValue * docSizeBytes,
                                                   collCardValue * docSizeBytes}}}};

        jCtx.emplace(makeContext());

        SubsetCardinalities subsetCards{{makeNodeSet(nodeId), makeCard(numDocsOutputValue)}};
        cardEstimator = std::make_unique<FakeJoinCardinalityEstimator>(*jCtx, subsetCards);

        costEstimator = std::make_unique<JoinCostEstimatorImpl>(*jCtx, *cardEstimator);
    }

    NamespaceString nss;
    NodeId nodeId;
    boost::optional<JoinReorderingContext> jCtx;
    std::unique_ptr<JoinCardinalityEstimator> cardEstimator;
    std::unique_ptr<JoinCostEstimator> costEstimator;
};

// Verify that a non-multikey index scan costs less when NDV < numDocsOutput. A lower NDV means
// fewer distinct sort-sparse IO groups, so numLogicalPageRequests (= NDV * selectivity) is smaller.
TEST_F(IndexScanNDVCostTest, LowNDVHasLowerCostThanHighNDV) {
    // Without sampling estimators the fallback is numLogicalPageRequests = numDocsOutput = 200.
    auto costWithoutNDV = costEstimator->costIndexScanFragment(nodeId, zeroCost);

    // With NDV = 50 (< numDocsOutput = 200):
    //   numLogicalPageRequests = 50 * 200 / 1000 = 10 → lower cost.
    auto fakeNdvEstimator = std::make_unique<FakeNdvEstimator>(makeCard(1000));
    fakeNdvEstimator->addFakeNDVEstimate({FieldPath("a")}, makeCard(50));
    SamplingEstimatorMap samplingEstimators;
    samplingEstimators.emplace(nss, std::move(fakeNdvEstimator));
    jCtx->samplingEstimators = &samplingEstimators;

    auto costWithLowNDV = costEstimator->costIndexScanFragment(nodeId, zeroCost);

    ASSERT_GT(costWithoutNDV, costWithLowNDV);
}

TEST_F(IndexScanNDVCostTest, LowNDVCostsMoreUnderCachePressure) {
    // An IXSCAN+FETCH on a low-NDV index fetches multiple docs per distinct key. Pages past the
    // first random seek per key are sorted-sparse I/Os: charged as cheap sequential I/Os when the
    // working set fits in cache, but as additional random I/Os under cache pressure. Verify the
    // fragment cost reflects that by forcing cache pressure and asserting it is strictly more
    // expensive than the same scan under a cache that comfortably fits the collection.
    auto fakeNdvEstimator = std::make_unique<FakeNdvEstimator>(makeCard(1000));
    fakeNdvEstimator->addFakeNDVEstimate({FieldPath("a")}, makeCard(50));
    SamplingEstimatorMap samplingEstimators;
    samplingEstimators.emplace(nss, std::move(fakeNdvEstimator));
    jCtx->samplingEstimators = &samplingEstimators;

    auto costFitsInCache = costEstimator->costIndexScanFragment(nodeId, zeroCost);
    jCtx->catStats.bytesInStorageEngineCache = 32 * 1024;
    auto costEviction = costEstimator->costIndexScanFragment(nodeId, zeroCost);

    ASSERT_LT(costFitsInCache, costEviction);
}

TEST(JoinEstimatesTest, NumDocsProcessedFromCpuCost) {
    ASSERT_EQ(0.0, numDocsProcessedFromCpuCost(zeroCost).toDouble());

    // The mapping is linear in the CPU cost: doubling the input exactly doubles the output.
    const CostEstimate cpu{CostType{1.0}, EstimationSource::Code};
    const double docsForCpu = numDocsProcessedFromCpuCost(cpu).toDouble();
    const double docsFor2xCpu = numDocsProcessedFromCpuCost(cpu * 2.0).toDouble();
    ASSERT_GT(docsForCpu, 0.0);
    ASSERT_EQ(2.0 * docsForCpu, docsFor2xCpu);
}

TEST(MackertLohmanTest, CollectionFitsInCache) {
    auto result1 = estimateMackertLohmanRandIO(100, 1000, 10);
    ASSERT_EQ(10, result1.randIOPages);
    ASSERT_EQ(MackertLohmanCase::kCollectionFitsCache, result1.theCase);

    auto result2 = estimateMackertLohmanRandIO(100, 1000, 10000);
    ASSERT_EQ(100, result2.randIOPages);
    ASSERT_EQ(MackertLohmanCase::kCollectionFitsCache, result2.theCase);
}

TEST(MackerLohmanTest, CollectionDoesntFitInCacheResultSetFitsInCache) {
    auto result = estimateMackertLohmanRandIO(1000, 100, 10);
    ASSERT_EQ(10, result.randIOPages);
    ASSERT_EQ(MackertLohmanCase::kReturnedDocsFitCache, result.theCase);
}

TEST(MackerLohmanTest, CollectionDoesntFitInCacheResultSetDoesntFitInCache) {
    auto result = estimateMackertLohmanRandIO(1000, 100, 1000);
    ASSERT_EQ(910, result.randIOPages);
    ASSERT_EQ(MackertLohmanCase::kPartialEviction, result.theCase);
}

TEST(SortedSparseIOTest, NoTailWhenLogicalRequestsExceedPages) {
    // When the number of logical page requests exceeds the number of distinct pages accessed there
    // is no sorted-sparse IO, so the helper returns {0, 0} regardless of the Mackert-Lohman
    // branch.
    auto result = estimateSortedSparseIO(10, 100, MackertLohmanCase::kPartialEviction);
    ASSERT_EQ(0, result.numSeqIOs);
    ASSERT_EQ(0, result.numRandIOs);
}

TEST(SortedSparseIOTest, CollectionFitsCacheChargesNothing) {
    auto result = estimateSortedSparseIO(100, 10, MackertLohmanCase::kCollectionFitsCache);
    ASSERT_EQ(0, result.numSeqIOs);
    ASSERT_EQ(0, result.numRandIOs);
}

TEST(SortedSparseIOTest, ReturnedDocsFitCacheChargesSequential) {
    auto result = estimateSortedSparseIO(100, 10, MackertLohmanCase::kReturnedDocsFitCache);
    ASSERT_EQ(135, result.numSeqIOs);
    ASSERT_EQ(0, result.numRandIOs);
}

TEST(SortedSparseIOTest, PartialEvictionChargesAdditionalRandom) {
    auto result = estimateSortedSparseIO(100, 10, MackertLohmanCase::kPartialEviction);
    ASSERT_EQ(0, result.numSeqIOs);
    ASSERT_EQ(90, result.numRandIOs);
}

}  // namespace mongo::join_ordering
