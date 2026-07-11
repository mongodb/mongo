// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counters.h"

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/util/assert_util.h"

namespace mongo::plan_shape_counters {
namespace {

// Returns the root of the find portion of the QuerySolution.
const QuerySolutionNode* getFindRoot(const QuerySolution& solution) {
    const QuerySolutionNode* node = solution.root();
    if (!solution.hasExtension()) {
        return node;
    }
    // The unextended root can be removed by some optimizations, so we can't
    // search for the exact node ID.
    while (node->nodeId() > solution.unextendedRootId()) {
        QuerySolutionNode::assertSupportsLeftMostBranchTraversal(node);
        tassert(11907602,
                "Extension chain ended before reaching the unextended root",
                !node->children.empty());
        const QuerySolutionNode* child = node->children[0].get();
        tassert(11907603,
                "Node ids do not strictly decrease down the extension branch",
                child->nodeId() < node->nodeId());
        node = child;
    }
    return node;
}
}  // namespace

PlanShapeAnalysisResult analyzePlanShapeForCounters(const QuerySolution& solution) {
    const QuerySolutionNode* findRoot = getFindRoot(solution);

    QsnNodeCountAnalyzer nodeCounts;
    const QuerySolutionNode* extensionNode = solution.root();
    // While we're in the extension portion, only call `QsnNodeCountAnalyzer`. The plan
    // shape matcher and access path matcher only operate on the find layer.
    while (extensionNode != findRoot) {
        QuerySolutionNode::assertSupportsLeftMostBranchTraversal(extensionNode);
        nodeCounts.preVisit(*extensionNode);
        tassert(13022300,
                "Expected extension node to have at least one child.",
                !extensionNode->children.empty());
        extensionNode = extensionNode->children[0].get();
    }

    // Now call all counters on the find layer.
    AccessPathAnalyzer accessPaths;
    auto planShapeMatcher = makePlanShapeMatcher();
    // Don't allow the treeSearch call to exit early, so that every rule gets a chance to
    // match on every node.
    const bool matched = treeMatchesAny(
        findRoot, false /* allowEarlyExit */, nodeCounts, accessPaths, planShapeMatcher);

    boost::optional<PlanShapeCounter> pattern;
    if (matched) {
        pattern = static_cast<PlanShapeCounter>(*planShapeMatcher.getMatchedTag());
    }
    return PlanShapeAnalysisResult{.pattern = pattern,
                                   .accessPathCounts = accessPaths.counts(),
                                   .qsnNodeCounts = nodeCounts.counts()};
}

}  // namespace mongo::plan_shape_counters
