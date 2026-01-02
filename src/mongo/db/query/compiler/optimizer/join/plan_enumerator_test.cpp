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

#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/golden_test.h"
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
    void initGraph(size_t numNodes, bool withIndexes = false) {
        for (size_t i = 0; i < numNodes; i++) {
            auto nss =
                NamespaceString::createNamespaceString_forTest("test", str::stream() << "nss" << i);
            std::string fieldName = str::stream() << "a" << i;
            auto filterBSON = bsonStorage.emplace_back(BSON(fieldName << BSON("$gt" << 0)));
            auto cq = makeCanonicalQuery(nss, filterBSON);
            cbrCqQsns.emplace(cq.get(),
                              makeCollScanPlan(nss, cq->getPrimaryMatchExpression()->clone()));
            ASSERT_TRUE(graph.addNode(nss, std::move(cq), boost::none).has_value());

            if (withIndexes) {
                perCollIdxs.emplace(nss,
                                    makeIndexCatalogEntries({BSON(fieldName << (i % 2 ? 1 : -1))}));
            }

            resolvedPaths.emplace_back(ResolvedPath{(NodeId)i, FieldPath(fieldName)});
        }
    }

    std::unique_ptr<JoinCardinalityEstimator> makeFakeEstimator(const JoinReorderingContext& jCtx) {
        return std::make_unique<FakeJoinCardinalityEstimator>(jCtx);
    }

    // Asserts that for all HJ enumerated at every level of enumeration, the CE for the LHS of the
    // HJ is smaller than the CE for the RHS. All other plans should have been pruned.
    void makeHJPruningAssertions(JoinReorderingContext& jCtx, PlanEnumeratorContext& ctx) {
        for (size_t level = 1; level < jCtx.joinGraph.numNodes(); level++) {
            for (const auto& subset : ctx.getSubsets(level)) {
                for (const auto& planId : subset.plans) {
                    const auto& plan = ctx.registry().getAs<JoiningNode>(planId);
                    if (plan.method != JoinMethod::HJ) {
                        continue;
                    }

                    const auto& left = ctx.registry().getBitset(plan.left);
                    const auto& right = ctx.registry().getBitset(plan.right);
                    ASSERT(
                        ctx.getJoinCardinalityEstimator()->getOrEstimateSubsetCardinality(left) <=
                        ctx.getJoinCardinalityEstimator()->getOrEstimateSubsetCardinality(right));
                }
            }
        }
    }

    void testLargeSubset(unittest::GoldenTestContext* goldenCtx,
                         PlanTreeShape shape,
                         size_t numNodes,
                         bool withIndexes = false) {
        initGraph(numNodes, withIndexes);

        for (size_t i = 1; i < numNodes; ++i) {
            // Make the graph fully connected in order to ensure we generate as many plans as
            // possible.
            for (size_t j = 0; j < i; ++j) {
                ASSERT_TRUE(graph.addSimpleEqualityEdge((NodeId)j, (NodeId)i, j, i).has_value());
            }
        }

        auto jCtx = makeContext();

        // Note: These tests run with pruning enabled to keep the large output understandable.
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), true /* HJ pruning */};
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

        if (shape == PlanTreeShape::ZIG_ZAG) {
            makeHJPruningAssertions(jCtx, ctx);
        }
    }

    std::vector<BSONObj> bsonStorage;
};

TEST_F(JoinPlanEnumeratorTest, InitializeSubsetsTwo) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    initGraph(2);
    graph.addSimpleEqualityEdge((NodeId)0, (NodeId)1, 0, 1);
    auto jCtx = makeContext();
    {
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), false /* disable HJ pruning */};
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
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), false /* disable HJ pruning */};
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
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 1, 2);

    auto jCtx = makeContext();

    {
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), false /* disable HJ pruning */};
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
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), false /* disable HJ pruning */};
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
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);

    auto jCtx = makeContext();
    {
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), false /* disable HJ pruning */};
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
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), false /* disable HJ pruning */};
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

TEST_F(JoinPlanEnumeratorTest, InitializeSubsetsThreeWithPruning) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);

    auto jCtx = makeContext();
    {
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), true /* HJ pruning */};
        ctx.enumerateJoinSubsets(PlanTreeShape::LEFT_DEEP);

        goldenCtx.outStream() << "LEFT DEEP, 3 Nodes with pruning" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }

    {
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), true /* HJ pruning */};
        ctx.enumerateJoinSubsets(PlanTreeShape::RIGHT_DEEP);

        goldenCtx.outStream() << "RIGHT DEEP, 3 Nodes with pruning" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;
    }

    {
        PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), true /* HJ pruning */};
        ctx.enumerateJoinSubsets(PlanTreeShape::ZIG_ZAG);

        goldenCtx.outStream() << "ZIG ZAG, 3 Nodes with pruning" << "\n";
        goldenCtx.outStream() << ctx.toString() << std::endl;

        makeHJPruningAssertions(jCtx, ctx);
    }
}

TEST_F(JoinPlanEnumeratorTest, InitializeSubsetsFourWithPruning) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);

    initGraph(4);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);
    graph.addSimpleEqualityEdge(NodeId(2), NodeId(3), 2, 3);

    auto jCtx = makeContext();
    PlanEnumeratorContext ctx{jCtx, makeFakeEstimator(jCtx), true /* HJ pruning */};
    ctx.enumerateJoinSubsets(PlanTreeShape::ZIG_ZAG);

    goldenCtx.outStream() << "ZIG ZAG, 4 Nodes with pruning" << "\n";
    goldenCtx.outStream() << ctx.toString() << std::endl;

    makeHJPruningAssertions(jCtx, ctx);
}

TEST_F(JoinPlanEnumeratorTest, LeftDeep8Nodes) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    testLargeSubset(&goldenCtx, PlanTreeShape::LEFT_DEEP, 8);
}

TEST_F(JoinPlanEnumeratorTest, LeftDeep8NodesINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    testLargeSubset(&goldenCtx, PlanTreeShape::LEFT_DEEP, 8, true /* withIndexes */);
}

TEST_F(JoinPlanEnumeratorTest, RightDeep8Nodes) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    testLargeSubset(&goldenCtx, PlanTreeShape::RIGHT_DEEP, 8);
}

TEST_F(JoinPlanEnumeratorTest, RightDeep8NodesINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    testLargeSubset(&goldenCtx, PlanTreeShape::RIGHT_DEEP, 8, true /* withIndexes */);
}

TEST_F(JoinPlanEnumeratorTest, ZigZag8Nodes) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    testLargeSubset(&goldenCtx, PlanTreeShape::ZIG_ZAG, 8);
}

TEST_F(JoinPlanEnumeratorTest, ZigZag8NodesINLJ) {
    unittest::GoldenTestContext goldenCtx(&goldenTestConfig);
    testLargeSubset(&goldenCtx, PlanTreeShape::ZIG_ZAG, 8, true /* withIndexes */);
}

TEST_F(JoinPlanEnumeratorTest, InitialzeLargeSubsets) {
    testLargeSubset(nullptr /* No golden test here. */, PlanTreeShape::LEFT_DEEP, 10);
}

}  // namespace mongo::join_ordering
