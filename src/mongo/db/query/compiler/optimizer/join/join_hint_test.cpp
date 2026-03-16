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

#include "mongo/db/query/compiler/optimizer/join/join_plan.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, NoModes, "11391600") {
    PerSubsetLevelEnumerationMode(std::vector<SubsetLevelMode>{});
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, FirstModeLevelNotZero, "11391600") {
    PerSubsetLevelEnumerationMode({{1, PlanEnumerationMode::ALL}});
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, SameModeConsecutively, "11391600") {
    PerSubsetLevelEnumerationMode({{0, PlanEnumerationMode::ALL}, {1, PlanEnumerationMode::ALL}});
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, SameModeConsecutively2, "11391600") {
    PerSubsetLevelEnumerationMode({{0, PlanEnumerationMode::ALL},
                                   {3, PlanEnumerationMode::CHEAPEST},
                                   {6, PlanEnumerationMode::CHEAPEST}});
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, NonAscendingMode, "11391600") {
    PerSubsetLevelEnumerationMode({{0, PlanEnumerationMode::ALL},
                                   {1, PlanEnumerationMode::CHEAPEST},
                                   {1, PlanEnumerationMode::ALL}});
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, NonAscendingMode2, "11391600") {
    PerSubsetLevelEnumerationMode({{0, PlanEnumerationMode::ALL},
                                   {5, PlanEnumerationMode::CHEAPEST},
                                   {4, PlanEnumerationMode::ALL}});
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, NonAscendingMode3, "11391600") {
    PerSubsetLevelEnumerationMode({
        {0, PlanEnumerationMode::ALL},
        {2, PlanEnumerationMode::CHEAPEST},
        {4, PlanEnumerationMode::ALL},
        {3, PlanEnumerationMode::CHEAPEST},
    });
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, HintedWithNoHints, "11391600") {
    PerSubsetLevelEnumerationMode({
        {0, PlanEnumerationMode::HINTED},
    });
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, HintedWithNoHints2, "11391600") {
    PerSubsetLevelEnumerationMode({
        {0, PlanEnumerationMode::CHEAPEST},
        {5, PlanEnumerationMode::HINTED},
    });
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, HintedWithNoHints3, "11391600") {
    PerSubsetLevelEnumerationMode({
        {0, PlanEnumerationMode::CHEAPEST},
        {.level = 3,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 1, .method = JoinMethod::HJ, .isLeftChild = true}},
        {4, PlanEnumerationMode::HINTED},  // Bad hint.
    });
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, HintedWithRepeatedNode, "11391600") {
    PerSubsetLevelEnumerationMode({
        {0, PlanEnumerationMode::CHEAPEST},
        {.level = 3,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 1, .method = JoinMethod::HJ, .isLeftChild = false}},
        {.level = 4,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 2, .method = JoinMethod::HJ, .isLeftChild = true}},
        {.level = 5,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 1, .method = JoinMethod::HJ, .isLeftChild = true}},  // Bad hint.
    });
}
DEATH_TEST(PerSubsetLevelEnumerationModeDeathTest, HintedWithLevelSkip, "11391600") {
    PerSubsetLevelEnumerationMode({
        {0, PlanEnumerationMode::CHEAPEST},
        {.level = 3,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 1, .method = JoinMethod::HJ, .isLeftChild = false}},
        {.level = 4,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 2, .method = JoinMethod::HJ, .isLeftChild = true}},
        {.level = 6,
         .mode = PlanEnumerationMode::HINTED,
         .hint = JoinHint{.node = 3, .method = JoinMethod::HJ, .isLeftChild = true}},  // Bad hint.
    });
}

class JoinPlanEnumeratorHintingTest : public JoinOrderingTestFixture {
public:
    PlanEnumeratorContext makeEnumeratorContext(const JoinReorderingContext& ctx,
                                                EnumerationStrategy strategy) {
        return {ctx, nullptr, nullptr, std::move(strategy)};
    }
    void validatePlanWasHintedCorrectly(const JoinReorderingContext& jCtx,
                                        EnumerationStrategy strat) {
        auto ctx = makeEnumeratorContext(jCtx, strat);
        ctx.enumerateJoinSubsets();
        const auto& registry = ctx.registry();

        // Validate we have all base nodes.
        auto it = strat.mode.begin();
        ASSERT_EQ(it.get().mode, PlanEnumerationMode::HINTED);
        ASSERT(it.get().hint);
        const auto firstHintNode = it.get().hint->node;
        ASSERT_EQ(ctx.getSubsets(0).size(), jCtx.joinGraph.numNodes());
        for (const auto& s : ctx.getSubsets(0)) {
            // We may have a base node AND an INLJ node.
            ASSERT_LTE(s.plans.size(), 2);
            ASSERT(registry.isOfType<BaseNode>(s.plans[0]));
            ASSERT_EQ(registry.getAs<BaseNode>(s.plans[0]).node, s.getNodeId());
            if (s.plans.size() > 1) {
                ASSERT(registry.isOfType<INLJRHSNode>(s.plans[1]));
                ASSERT_EQ(registry.getAs<INLJRHSNode>(s.plans[1]).node, s.getNodeId());
            }
        }
        it.next();

        // Validate we have exactly one subset and one join node per subset per level after this.
        // Furthermore, ensure that each such plan matches its corresponding hint.
        for (size_t i = 1; i < jCtx.joinGraph.numNodes(); i++) {
            ASSERT_EQ(it.get().mode, PlanEnumerationMode::HINTED);
            ASSERT(it.get().hint);

            const auto& s = ctx.getSubsets(i);
            ASSERT_EQ(s.size(), 1);
            ASSERT_EQ(s[0].plans.size(), 1);
            ASSERT(registry.isOfType<JoiningNode>(s[0].plans[0]));
            const auto& j = registry.getAs<JoiningNode>(s[0].plans[0]);
            ASSERT_EQ(j.method, it.get().hint->method);

            if (it.get().hint->isLeftChild) {
                // Only a HJ can have a left child be a base collection access.
                ASSERT(j.method == JoinMethod::HJ);
            }

            const JoinPlanNodeId child = it.get().hint->isLeftChild ? j.left : j.right;
            if (j.method != JoinMethod::INLJ) {
                ASSERT(registry.isOfType<BaseNode>(child));
                ASSERT_EQ(registry.getAs<BaseNode>(child).node, it.get().hint->node);
            } else {
                ASSERT(registry.isOfType<INLJRHSNode>(child));
                ASSERT_EQ(registry.getAs<INLJRHSNode>(child).node, it.get().hint->node);
            }

            if (i == 1) {
                // Validate both children of node for the join level.
                const JoinPlanNodeId otherChild = it.get().hint->isLeftChild ? j.right : j.left;
                if (j.method != JoinMethod::INLJ || !it.get().hint->isLeftChild) {
                    ASSERT(registry.isOfType<BaseNode>(otherChild));
                    ASSERT_EQ(registry.getAs<BaseNode>(otherChild).node, firstHintNode);
                } else {
                    ASSERT(registry.isOfType<INLJRHSNode>(otherChild));
                    ASSERT_EQ(registry.getAs<INLJRHSNode>(otherChild).node, firstHintNode);
                }
            }

            it.next();
        }
    }
};

TEST_F(JoinPlanEnumeratorHintingTest, MultiEnumerationModes) {
    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 1, 2);

    auto jCtx = makeContext();
    {
        auto ctx =
            makeEnumeratorContext(jCtx,
                                  EnumerationStrategy{.planShape = PlanTreeShape::ZIG_ZAG,
                                                      .mode = PerSubsetLevelEnumerationMode(
                                                          {{0, PlanEnumerationMode::ALL},
                                                           {1, PlanEnumerationMode::CHEAPEST},
                                                           {2, PlanEnumerationMode::ALL}}),
                                                      .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();

        auto& level0 = ctx.getSubsets(0);
        // 3 nodes => 3 base collection accesses (regardless of mode).
        ASSERT_EQ(level0.size(), 3);
        for (auto& subset : level0) {
            ASSERT_EQ(subset.plans.size(), 1);
        }

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(level1.size(), 3);
        size_t totalPlans = 0;
        for (auto& subset : level1) {
            // Use cheapest enumeration mode => our "best plan" is always the last one enumerated.
            // Depending on what's cheapest, we may have more/fewer plans. In this case, however, we
            // enumerate the best plan first, so we only have one per subset.
            ASSERT_EQ(subset.plans.size(), 1);
            totalPlans += subset.plans.size();
        }
        // In all-plans enumeration mode, we would expect more plans.
        ASSERT_EQ(totalPlans, 3);

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(level2.size(), 1);  // Only one subset left.
        // Use ALL enumeration mode => every pair of plans generates 2HJ + 1NLJ (RHS must be
        // base collection for NLJ), and we can enumerate all pairs of plans.
        ASSERT_EQ(level2[0].plans.size(), 3 * totalPlans * (totalPlans - 1) / 2);
    }

    {
        auto ctx =
            makeEnumeratorContext(jCtx,
                                  EnumerationStrategy{.planShape = PlanTreeShape::ZIG_ZAG,
                                                      .mode = PerSubsetLevelEnumerationMode(
                                                          {{0, PlanEnumerationMode::CHEAPEST},
                                                           {1, PlanEnumerationMode::ALL},
                                                           {2, PlanEnumerationMode::CHEAPEST}}),
                                                      .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();

        auto& level0 = ctx.getSubsets(0);
        // 3 nodes => 3 base collection accesses (regardless of mode).
        ASSERT_EQ(level0.size(), 3);
        for (auto& subset : level0) {
            ASSERT_EQ(subset.plans.size(), 1);
        }

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(level1.size(), 3);
        for (auto& subset : level1) {
            // Enumerate up to 2HJ + 2NLJ per subset.
            ASSERT_EQ(subset.plans.size(), 4);
        }

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(level2.size(), 1);  // Only one subset left.
        // Use CHEAPEST enumeration mode => best plan is always the last one we enumerated.
        ASSERT_EQ(level2[0].plans.size(), level2[0].bestPlanIndex + 1);
    }

    {
        auto ctx =
            makeEnumeratorContext(jCtx,
                                  EnumerationStrategy{.planShape = PlanTreeShape::ZIG_ZAG,
                                                      .mode = PerSubsetLevelEnumerationMode(
                                                          {{0, PlanEnumerationMode::CHEAPEST},
                                                           {2, PlanEnumerationMode::ALL}}),
                                                      .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();

        auto& level0 = ctx.getSubsets(0);
        // 3 nodes => 3 base collection accesses (regardless of mode).
        ASSERT_EQ(level0.size(), 3);
        for (auto& subset : level0) {
            ASSERT_EQ(subset.plans.size(), 1);
        }

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(level1.size(), 3);
        size_t totalPlans = 0;
        for (auto& subset : level1) {
            ASSERT_EQ(subset.plans.size(), 1);
            totalPlans += subset.plans.size();
        }
        ASSERT_EQ(totalPlans, 3);

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(level2.size(), 1);  // Only one subset left.
        // Use ALL enumeration mode => every pair of plans generates 2HJ + 1NLJ (RHS must be
        // base collection for NLJ), and we can enumerate all pairs of plans.
        ASSERT_EQ(level2[0].plans.size(), 3 * totalPlans * (totalPlans - 1) / 2);
    }

    {
        auto ctx =
            makeEnumeratorContext(jCtx,
                                  EnumerationStrategy{.planShape = PlanTreeShape::ZIG_ZAG,
                                                      .mode = PerSubsetLevelEnumerationMode(
                                                          {{0, PlanEnumerationMode::ALL},
                                                           {2, PlanEnumerationMode::CHEAPEST}}),
                                                      .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();

        auto& level0 = ctx.getSubsets(0);
        // 3 nodes => 3 base collection accesses (regardless of mode).
        ASSERT_EQ(level0.size(), 3);
        for (auto& subset : level0) {
            ASSERT_EQ(subset.plans.size(), 1);
        }

        auto& level1 = ctx.getSubsets(1);
        ASSERT_EQ(level1.size(), 3);
        for (auto& subset : level1) {
            // ALL => enumerate 2HJ + 2NLJ per subset.
            ASSERT_EQ(subset.plans.size(), 4);
        }

        auto& level2 = ctx.getSubsets(2);
        ASSERT_EQ(level2.size(), 1);  // Only one subset left.
        // Best plan must be last plan enumerated.
        ASSERT_EQ(level2[0].plans.size(), level2[0].bestPlanIndex + 1);
    }
}

TEST_F(JoinPlanEnumeratorHintingTest, HintedEnumeration) {
    initGraph(3);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 1, 2);

    auto jCtx = makeContext();
    validatePlanWasHintedCorrectly(
        jCtx,
        EnumerationStrategy{
            .planShape = PlanTreeShape::ZIG_ZAG,
            .mode = PerSubsetLevelEnumerationMode({
                {.level = 0,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 1, .method = JoinMethod::HJ, .isLeftChild = true}},
                {.level = 1,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 2, .method = JoinMethod::NLJ, .isLeftChild = false}},
                {.level = 2,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 0, .method = JoinMethod::HJ, .isLeftChild = true}},
            }),
            .enableHJOrderPruning = false});

    validatePlanWasHintedCorrectly(
        jCtx,
        EnumerationStrategy{
            .planShape = PlanTreeShape::LEFT_DEEP,
            .mode = PerSubsetLevelEnumerationMode({
                {.level = 0,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 0, .method = JoinMethod::NLJ, .isLeftChild = false}},
                {.level = 1,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 2, .method = JoinMethod::HJ, .isLeftChild = false}},
                {.level = 2,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 1, .method = JoinMethod::NLJ, .isLeftChild = false}},
            }),
            .enableHJOrderPruning = false});

    // Can't enumerate INLJ without index information.
    {
        auto ctx = makeEnumeratorContext(
            jCtx,
            EnumerationStrategy{
                .planShape = PlanTreeShape::LEFT_DEEP,
                .mode = PerSubsetLevelEnumerationMode({
                    {.level = 0,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 0, .method = JoinMethod::NLJ, .isLeftChild = false}},
                    {.level = 1,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 2, .method = JoinMethod::NLJ, .isLeftChild = false}},
                    {.level = 2,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 1, .method = JoinMethod::INLJ, .isLeftChild = false}},
                }),
                .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();
        ASSERT_FALSE(ctx.enumerationSuccessful());
    }

    // Can't enumerate a LEFT_DEEP plan in RIGHT_DEEP mode.
    {
        auto ctx = makeEnumeratorContext(
            jCtx,
            EnumerationStrategy{
                .planShape = PlanTreeShape::RIGHT_DEEP,
                .mode = PerSubsetLevelEnumerationMode({
                    {.level = 0,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 0, .method = JoinMethod::NLJ, .isLeftChild = false}},
                    {.level = 1,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 2, .method = JoinMethod::NLJ, .isLeftChild = false}},
                    {.level = 2,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 1, .method = JoinMethod::NLJ, .isLeftChild = false}},
                }),
                .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();
        ASSERT_FALSE(ctx.enumerationSuccessful());
    }
}

TEST_F(JoinPlanEnumeratorHintingTest, HintedEnumerationINLJ) {
    initGraph(2, true /* withIndexes */);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);

    auto jCtx = makeContext();
    validatePlanWasHintedCorrectly(
        jCtx,
        EnumerationStrategy{
            .planShape = PlanTreeShape::ZIG_ZAG,
            .mode = PerSubsetLevelEnumerationMode({
                {.level = 0,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 0, .method = JoinMethod::INLJ, .isLeftChild = true}},
                {.level = 1,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 1, .method = JoinMethod::INLJ, .isLeftChild = false}},
            }),
            .enableHJOrderPruning = false});
    validatePlanWasHintedCorrectly(
        jCtx,
        EnumerationStrategy{
            .planShape = PlanTreeShape::RIGHT_DEEP,
            .mode = PerSubsetLevelEnumerationMode({
                {.level = 0,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 1, .method = JoinMethod::INLJ, .isLeftChild = false}},
                {.level = 1,
                 .mode = PlanEnumerationMode::HINTED,
                 .hint = JoinHint{.node = 0, .method = JoinMethod::INLJ, .isLeftChild = false}},
            }),
            .enableHJOrderPruning = false});
}

TEST_F(JoinPlanEnumeratorHintingTest, HintedEnumerationNoContradictoryShapes) {
    initGraph(3, true /* withIndexes */);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(1), 0, 1);
    graph.addSimpleEqualityEdge(NodeId(1), NodeId(2), 1, 2);
    graph.addSimpleEqualityEdge(NodeId(0), NodeId(2), 0, 2);

    auto jCtx = makeContext();
    // Can't enumerate an INLJ-only plan if base nodes all on left.
    {
        auto ctx = makeEnumeratorContext(
            jCtx,
            EnumerationStrategy{
                .planShape = PlanTreeShape::ZIG_ZAG,
                .mode = PerSubsetLevelEnumerationMode({
                    {.level = 0,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 0, .method = JoinMethod::INLJ, .isLeftChild = true}},
                    {.level = 1,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 2, .method = JoinMethod::INLJ, .isLeftChild = true}},
                    {.level = 2,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 1, .method = JoinMethod::INLJ, .isLeftChild = true}},
                }),
                .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();
        ASSERT_FALSE(ctx.enumerationSuccessful());
    }
    // If we set the shape, we still can't enumerate if contradicts with 'isLeftChild'.
    {
        auto ctx = makeEnumeratorContext(
            jCtx,
            EnumerationStrategy{
                .planShape = PlanTreeShape::LEFT_DEEP,
                .mode = PerSubsetLevelEnumerationMode({
                    {.level = 0,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 0, .method = JoinMethod::HJ, .isLeftChild = true}},
                    {.level = 1,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 2, .method = JoinMethod::HJ, .isLeftChild = true}},
                    {.level = 2,
                     .mode = PlanEnumerationMode::HINTED,
                     .hint = JoinHint{.node = 1, .method = JoinMethod::HJ, .isLeftChild = true}},
                }),
                .enableHJOrderPruning = false});
        ctx.enumerateJoinSubsets();
        ASSERT_FALSE(ctx.enumerationSuccessful());
    }
}

}  // namespace mongo::join_ordering
