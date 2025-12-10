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

#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"

#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

TEST(PlanEnumeratorHelpers, CombinationsEdgeCases) {
    ASSERT_EQ(1, combinations(0, 0));
    ASSERT_EQ(0, combinations(0, -1));
    ASSERT_EQ(0, combinations(0, 1));
    ASSERT_EQ(1, combinations(1, 0));
    ASSERT_EQ(0, combinations(-1, 0));
    ASSERT_EQ(0, combinations(-1, -1));
    ASSERT_EQ(0, combinations(1, 2));
    ASSERT_EQ(0, combinations(-1, 2));
    ASSERT_EQ(0, combinations(1, -2));
}

TEST(PlanEnumeratorHelpers, Combinations) {
    // Known small values
    ASSERT_EQ(1, combinations(5, 0));
    ASSERT_EQ(5, combinations(5, 1));
    ASSERT_EQ(10, combinations(5, 2));
    ASSERT_EQ(10, combinations(5, 3));
    ASSERT_EQ(5, combinations(5, 4));
    ASSERT_EQ(1, combinations(5, 5));

    // Symmetry check
    ASSERT_EQ(combinations(10, 3), combinations(10, 7));

    // Known large value
    ASSERT_EQ(184756, combinations(20, 10));
}

TEST(PlanEnumeratorHelpers, CombinationSequence) {
    CombinationSequence cs(5);
    ASSERT_EQ(1, cs.next());
    ASSERT_EQ(5, cs.next());
    ASSERT_EQ(10, cs.next());
    ASSERT_EQ(10, cs.next());
    ASSERT_EQ(5, cs.next());
    ASSERT_EQ(1, cs.next());
}

DEATH_TEST(PlanEnumeratorHelpersDeathTest, TooManyInvocationsOfCombinationSequence, "10986301") {
    CombinationSequence cs(5);
    for (int i = 0; i < 6; ++i) {
        cs.next();
    }
    cs.next();  // tasserts
}

class JoinPlanEnumeratorTest : public JoinOrderingTestFixture {
public:
    JoinPlanEnumeratorTest() : config{"src/mongo/db/test_output/query/join"} {}

    void initGraph(size_t numNodes) {
        for (size_t i = 0; i < numNodes; i++) {
            auto nss =
                NamespaceString::createNamespaceString_forTest("test", str::stream() << "nss" << i);
            std::string fieldName = str::stream() << "a" << i;
            auto filterBSON = BSON(fieldName << BSON("$gt" << 0));
            auto cq = makeCanonicalQuery(nss, filterBSON);
            solnsPerQuery.insert(
                {cq.get(), makeCollScanPlan(nss, cq->getPrimaryMatchExpression()->clone())});
            ASSERT_TRUE(graph.addNode(nss, std::move(cq), boost::none).has_value());
        }
    }

    void testLargeSubset(unittest::GoldenTestContext* goldenCtx,
                         PlanTreeShape shape,
                         size_t numNodes) {
        initGraph(numNodes);

        for (size_t i = 1; i < numNodes; ++i) {
            // Make the graph fully connected in order to ensure we generate as many plans as
            // possible.
            for (size_t j = 0; j < i; ++j) {
                ASSERT_TRUE(graph.addSimpleEqualityEdge((NodeId)j, (NodeId)i, 0, 1).has_value());
            }
        }

        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(shape);
        ASSERT_EQ(numNodes, ctx.getSubsets(0).size());
        for (size_t k = 1; k < numNodes; ++k) {
            // The expected number of subsets for the k'th level is N choose k+1 (binomial
            // coefficient).
            size_t expectedLevelSize = combinations(numNodes, k + 1);
            auto& subsets = ctx.getSubsets(k);
            ASSERT_EQ(expectedLevelSize, subsets.size());
            for (auto&& s : subsets) {
                ASSERT_EQ(k + 1, s.subset.count());
            }
        }

        if (goldenCtx) {
            goldenCtx->outStream() << ctx.toString() << std::endl;
        }
    }

    unittest::GoldenTestConfig config;
    QuerySolutionMap solnsPerQuery;
    JoinGraph graph;
};

TEST_F(JoinPlanEnumeratorTest, InitializeSubsetsTwo) {
    unittest::GoldenTestContext goldenCtx(&config);

    initGraph(2);
    graph.addSimpleEqualityEdge((NodeId)0, (NodeId)1, 0, 1);

    {
        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(PlanTreeShape::LEFT_DEEP);

        auto& level0 = ctx.getSubsets(0);
        ASSERT_EQ(2, level0.size());
        ASSERT_EQ(NodeSet{"01"}, level0[0].subset);
        ASSERT_EQ(NodeSet{"10"}, level0[1].subset);

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(1, level1.size());
        ASSERT_EQ(NodeSet{"11"}, level1[0].subset);

        goldenCtx.outStream() << "LEFT DEEP, 2 Nodes" << "\n";
        goldenCtx.outStream() << ctx.toString() << "\n" << std::endl;
    }

    {
        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(PlanTreeShape::RIGHT_DEEP);

        auto& level0 = ctx.getSubsets(0);
        ASSERT_EQ(2, level0.size());
        ASSERT_EQ(NodeSet{"01"}, level0[0].subset);
        ASSERT_EQ(NodeSet{"10"}, level0[1].subset);

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(1, level1.size());
        ASSERT_EQ(NodeSet{"11"}, level1[0].subset);

        goldenCtx.outStream() << "RIGHT DEEP, 2 Nodes" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }
}

TEST_F(JoinPlanEnumeratorTest, InitializeSubsetsThree) {
    unittest::GoldenTestContext goldenCtx(&config);

    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 1, 2);

    {
        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(PlanTreeShape::LEFT_DEEP);

        auto& level0 = ctx.getSubsets(0);
        ASSERT_EQ(3, level0.size());
        ASSERT_EQ(NodeSet{"001"}, level0[0].subset);
        ASSERT_EQ(NodeSet{"010"}, level0[1].subset);
        ASSERT_EQ(NodeSet{"100"}, level0[2].subset);

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(3, level1.size());
        ASSERT_EQ(NodeSet{"011"}, level1[0].subset);
        ASSERT_EQ(NodeSet{"101"}, level1[1].subset);
        ASSERT_EQ(NodeSet{"110"}, level1[2].subset);

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(1, level2.size());
        ASSERT_EQ(NodeSet{"111"}, level2[0].subset);

        goldenCtx.outStream() << "LEFT DEEP, 3 Nodes" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }

    {
        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(PlanTreeShape::RIGHT_DEEP);

        auto& level0 = ctx.getSubsets(0);
        ASSERT_EQ(3, level0.size());
        ASSERT_EQ(NodeSet{"001"}, level0[0].subset);
        ASSERT_EQ(NodeSet{"010"}, level0[1].subset);
        ASSERT_EQ(NodeSet{"100"}, level0[2].subset);

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(3, level1.size());
        ASSERT_EQ(NodeSet{"011"}, level1[0].subset);
        ASSERT_EQ(NodeSet{"101"}, level1[1].subset);
        ASSERT_EQ(NodeSet{"110"}, level1[2].subset);

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(1, level2.size());
        ASSERT_EQ(NodeSet{"111"}, level2[0].subset);

        goldenCtx.outStream() << "RIGHT DEEP, 3 Nodes" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }
}

TEST_F(JoinPlanEnumeratorTest, InitializeSubsetsThreeNoCycle) {
    unittest::GoldenTestContext goldenCtx(&config);

    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);

    {
        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(PlanTreeShape::LEFT_DEEP);

        auto& level0 = ctx.getSubsets(0);
        ASSERT_EQ(3, level0.size());
        ASSERT_EQ(NodeSet{"001"}, level0[0].subset);
        ASSERT_EQ(NodeSet{"010"}, level0[1].subset);
        ASSERT_EQ(NodeSet{"100"}, level0[2].subset);

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(3, level1.size());
        ASSERT_EQ(NodeSet{"011"}, level1[0].subset);
        ASSERT_EQ(NodeSet{"101"}, level1[1].subset);
        ASSERT_EQ(NodeSet{"110"}, level1[2].subset);

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(1, level2.size());
        ASSERT_EQ(NodeSet{"111"}, level2[0].subset);

        goldenCtx.outStream() << "LEFT DEEP, 3 Nodes" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }

    {
        PlanEnumeratorContext ctx{graph, solnsPerQuery};
        ctx.enumerateJoinSubsets(PlanTreeShape::RIGHT_DEEP);

        auto& level0 = ctx.getSubsets(0);
        ASSERT_EQ(3, level0.size());
        ASSERT_EQ(NodeSet{"001"}, level0[0].subset);
        ASSERT_EQ(NodeSet{"010"}, level0[1].subset);
        ASSERT_EQ(NodeSet{"100"}, level0[2].subset);

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(3, level1.size());
        ASSERT_EQ(NodeSet{"011"}, level1[0].subset);
        ASSERT_EQ(NodeSet{"101"}, level1[1].subset);
        ASSERT_EQ(NodeSet{"110"}, level1[2].subset);

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(1, level2.size());
        ASSERT_EQ(NodeSet{"111"}, level2[0].subset);

        goldenCtx.outStream() << "RIGHT DEEP, 3 Nodes" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }
}

TEST_F(JoinPlanEnumeratorTest, LeftDeep8Nodes) {
    unittest::GoldenTestContext goldenCtx(&config);
    testLargeSubset(&goldenCtx, PlanTreeShape::LEFT_DEEP, 8);
}

TEST_F(JoinPlanEnumeratorTest, RightDeep8Nodes) {
    unittest::GoldenTestContext goldenCtx(&config);
    testLargeSubset(&goldenCtx, PlanTreeShape::RIGHT_DEEP, 8);
}

TEST_F(JoinPlanEnumeratorTest, ZigZag8Nodes) {
    unittest::GoldenTestContext goldenCtx(&config);
    testLargeSubset(&goldenCtx, PlanTreeShape::ZIG_ZAG, 8);
}

TEST_F(JoinPlanEnumeratorTest, InitialzeLargeSubsets) {
    testLargeSubset(nullptr /* No golden test here. */, PlanTreeShape::LEFT_DEEP, 15);
}

using JoinPredicateEstimatorFixture = JoinOrderingTestFixture;
using namespace cost_based_ranker;

// Join graph: A -- B with edge A.foo = B.foo and 'A' being the main collection
// The cardinality estimate for 'A' is smaller, so we assert that we use NDV(A.foo) for the join
// predicate selectivity estimate.
TEST_F(JoinPredicateEstimatorFixture, NDVSmallerCollection) {
    JoinGraph graph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});

    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});

    JoinPredicateEstimator predEstimator{graph, paths, samplingEstimators};

    auto selEst = predEstimator.joinPredicateSel(graph.getEdge(0));
    // The selectivity estimate comes from 1 / NDV(A.foo) = 1 / 5 = 0.2
    auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
    ASSERT_EQ(expectedSel, selEst);
}

// Join graph: A -- B with edge A.foo = B.foo and 'A' being the main collection
// The cardinality estimate for 'B' is smaller, so we assert that we use NDV(B.foo) for the join
// predicate selectivity estimate. This verifies that an embedded node can still be used for join
// predicate estimatation.
TEST_F(JoinPredicateEstimatorFixture, NDVSmallerCollectionEmbedPath) {
    JoinGraph graph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});

    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);

    SamplingEstimatorMap samplingEstimators;
    samplingEstimators[aNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});
    // Ensure "b" collection has smaller CE. Only add fake estimates for "b" estimator.
    auto bSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    bSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    samplingEstimators[bNss] = std::move(bSamplingEstimator);

    JoinPredicateEstimator predEstimator{graph, paths, samplingEstimators};

    auto selEst = predEstimator.joinPredicateSel(graph.getEdge(0));
    // The selectivity estimate comes from 1 / NDV(B.foo) = 1 / 5 = 0.2
    auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
    ASSERT_EQ(expectedSel, selEst);
}

// Join graph: A -- B with compound edge A.foo = B.foo && A.bar = B.bar and 'A' being the main
// collection. The cardinality estimate for 'A' is smaller, so we assert that we use the tuple
// NDV(A.foo, A.bar) for the join predicate selectivity estimate.
TEST_F(JoinPredicateEstimatorFixture, NDVCompoundJoinKey) {
    JoinGraph graph;
    auto aNss = NamespaceString::createNamespaceString_forTest("a");
    auto bNss = NamespaceString::createNamespaceString_forTest("b");
    auto aCQ = makeCanonicalQuery(aNss);
    auto bCQ = makeCanonicalQuery(bNss);
    auto aNodeId = *graph.addNode(aNss, std::move(aCQ), boost::none);
    auto bNodeId = *graph.addNode(bNss, std::move(bCQ), FieldPath{"b"});

    std::vector<ResolvedPath> paths;
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "foo"});
    paths.push_back(ResolvedPath{.nodeId = aNodeId, .fieldName = "bar"});
    paths.push_back(ResolvedPath{.nodeId = bNodeId, .fieldName = "bar"});

    // a.foo = b.foo && a.bar = b.bar
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 0, 1);
    graph.addSimpleEqualityEdge(aNodeId, bNodeId, 2, 3);

    SamplingEstimatorMap samplingEstimators;
    auto aSamplingEstimator = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{10}, EstimationSource::Sampling});
    // We should end up using the NDV from (foo, bar) and not from foo or bar.
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo"), FieldPath("bar")},
        CardinalityEstimate{CardinalityType{5}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("foo")}, CardinalityEstimate{CardinalityType{2}, EstimationSource::Sampling});
    aSamplingEstimator->addFakeNDVEstimate(
        {FieldPath("bar")}, CardinalityEstimate{CardinalityType{3}, EstimationSource::Sampling});
    samplingEstimators[aNss] = std::move(aSamplingEstimator);
    samplingEstimators[bNss] = std::make_unique<FakeNdvEstimator>(
        CardinalityEstimate{CardinalityType{20}, EstimationSource::Sampling});

    JoinPredicateEstimator predEstimator{graph, paths, samplingEstimators};

    auto selEst = predEstimator.joinPredicateSel(graph.getEdge(0));
    // The selectivity estimate comes from 1 / NDV(A.foo, A.bar) = 1 / 5 = 0.2
    auto expectedSel = SelectivityEstimate{SelectivityType{0.2}, EstimationSource::Sampling};
    ASSERT_EQ(expectedSel, selEst);
}

}  // namespace mongo::join_ordering
