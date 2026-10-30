// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner.h"

#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner_test_fixture.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/required_distribution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo::parallelism_planner {
namespace {

class ParallelismPlannerPlanTest : public ParallelismPlannerTest {
protected:
    bool shouldParallelizeTree(std::unique_ptr<QuerySolutionNode> root) const {
        return shouldParallelize(*solutionWithRoot(std::move(root)), _parallelCtx);
    }

    static void assertEveryEdgeSatisfiesItsRequirement(const QuerySolutionNode& node) {
        const auto reqs = childRequirements(node);
        ASSERT_EQ(reqs.size(), node.children.size());
        for (size_t i = 0; i < reqs.size(); ++i) {
            ASSERT_TRUE(satisfies(node.children[i]->physicalProps.distribution, reqs[i]))
                << nodeStageTypeToString(&node) << " child " << i;
            assertEveryEdgeSatisfiesItsRequirement(*node.children[i]);
        }
    }
};

using ShouldParallelizeTest = ParallelismPlannerPlanTest;
using ParallelismPlannerDeathTest = ParallelismPlannerPlanTest;


TEST_F(ShouldParallelizeTest, CollectionScanAndPassThroughNodesAreEligible) {
    ASSERT_TRUE(shouldParallelizeTree(scan()));
    ASSERT_TRUE(shouldParallelizeTree(matchOver(scan())));
    ASSERT_TRUE(shouldParallelizeTree(matchOver(matchOver(scan()))));
}

TEST_F(ShouldParallelizeTest, AnyUnsupportedNodeMakesTheWholeTreeIneligible) {
    ASSERT_FALSE(shouldParallelizeTree(ineligibleOver(scan())));
    // Below a supported node, and not just at the root.
    ASSERT_FALSE(shouldParallelizeTree(matchOver(ineligibleOver(scan()))));
}

TEST_F(ShouldParallelizeTest, DeclinesWhenMaxDoPIsOne) {
    auto solution = solutionWithRoot(scan());
    ASSERT_FALSE(shouldParallelize(*solution, _serialCtx));
    ASSERT_TRUE(shouldParallelize(*solution, _parallelCtx));
}

TEST_F(ShouldParallelizeTest, DeclinesOnUnsupportedNodes) {
    auto solution = solutionWithRoot(matchOver(ineligibleOver(scan())));
    ASSERT_FALSE(shouldParallelize(*solution, _parallelCtx));
}

TEST_F(ShouldParallelizeTest, DeclinesWithoutARoot) {
    QuerySolution solution;
    ASSERT_FALSE(shouldParallelize(solution, _parallelCtx));
}

TEST_F(ParallelismPlannerPlanTest, CollectionScanIsGatheredAtTheRoot) {
    auto solution = solutionWithRoot(scan());
    buildParallelPlan(*solution, _parallelCtx);

    ASSERT_EQ(solution->root()->getType(), STAGE_EXCHANGE);
    const auto& gather = static_cast<const ExchangeNode&>(*solution->root());
    ASSERT_TRUE(gather.isGather());
    ASSERT_FALSE(std::get<ExchangeRouting::Gather>(gather.routing.mode).sortPreserving);
    ASSERT_EQ(gather.physicalProps.distribution.type, DistributionDesc::Type::Unpartitioned);

    const auto* collScan = gather.children[0].get();
    ASSERT_EQ(collScan->getType(), STAGE_COLLSCAN);
    ASSERT_EQ(collScan->physicalProps.distribution.type, DistributionDesc::Type::RoundRobin);
}

TEST_F(ParallelismPlannerPlanTest, PassThroughNodesStayInsideTheParallelSection) {
    auto solution = solutionWithRoot(matchOver(scan()));
    buildParallelPlan(*solution, _parallelCtx);

    // GATHER -> MATCH -> COLLSCAN: the filter runs below the gather.
    ASSERT_EQ(solution->root()->getType(), STAGE_EXCHANGE);
    const auto* filter = solution->root()->children[0].get();
    ASSERT_EQ(filter->getType(), STAGE_MATCH);
    ASSERT_EQ(filter->physicalProps.distribution.type, DistributionDesc::Type::RoundRobin);
    ASSERT_EQ(filter->children[0]->getType(), STAGE_COLLSCAN);
}

TEST_F(ParallelismPlannerPlanTest, EveryEdgeSatisfiesItsRequirementAfterPlanning) {
    std::vector<std::unique_ptr<QuerySolutionNode>> roots;
    roots.push_back(scan());
    roots.push_back(matchOver(scan()));
    roots.push_back(matchOver(matchOver(scan())));
    for (auto& root : roots) {
        auto solution = solutionWithRoot(std::move(root));
        buildParallelPlan(*solution, _parallelCtx);
        assertEveryEdgeSatisfiesItsRequirement(*solution->root());
    }
}

DEATH_TEST_REGEX_F(ParallelismPlannerDeathTest, BuildRejectsSerialContext, "13623301") {
    auto solution = solutionWithRoot(scan());
    buildParallelPlan(*solution, _serialCtx);
}

DEATH_TEST_REGEX_F(ParallelismPlannerDeathTest, BuildRejectsIneligibleTree, "13623301") {
    auto solution = solutionWithRoot(matchOver(ineligibleOver(scan())));
    buildParallelPlan(*solution, _parallelCtx);
}

}  // namespace
}  // namespace mongo::parallelism_planner
