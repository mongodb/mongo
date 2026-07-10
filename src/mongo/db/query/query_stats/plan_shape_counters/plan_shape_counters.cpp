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
