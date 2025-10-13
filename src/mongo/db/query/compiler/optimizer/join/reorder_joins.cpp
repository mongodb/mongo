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

#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"

#include "mongo/db/query/random_utils.h"

namespace mongo::join_ordering {

namespace {
class ReorderContext {
public:
    ReorderContext(const JoinGraph& joinGraph, const std::vector<ResolvedPath>& resolvedPaths)
        : _joinGraph(joinGraph), _resolvedPaths(resolvedPaths) {}

    ReorderContext(const ReorderContext&) = delete;
    ReorderContext(ReorderContext&&) = delete;
    ReorderContext& operator=(const ReorderContext&) = delete;
    ReorderContext& operator=(ReorderContext&&) = delete;

    NodeId findMainCollectionNode() {
        for (size_t i = 0; i < _joinGraph.numNodes(); i++) {
            if (_joinGraph.getNode(i).embedPath == boost::none) {
                return i;
            }
        }
        MONGO_UNREACHABLE_TASSERT(11075701);
    }

    QSNJoinPredicate makePhysicalPredicate(JoinPredicate pred) {
        QSNJoinPredicate::ComparisonOp op = [&pred] {
            switch (pred.op) {
                case JoinPredicate::Eq:
                    return QSNJoinPredicate::ComparisonOp::Eq;
            }
            MONGO_UNREACHABLE_TASSERT(11075702);
        }();
        return {.op = op,
                .leftField = _resolvedPaths[pred.left].fieldName,
                .rightField = _resolvedPaths[pred.right].fieldName};
    }

    std::vector<QSNJoinPredicate> makeJoinPreds(const std::vector<EdgeId>& edges) {
        std::vector<QSNJoinPredicate> preds;
        preds.reserve(edges.size());
        for (auto&& edgeId : edges) {
            for (auto&& pred : _joinGraph.getEdge(edgeId).predicates) {
                preds.push_back(makePhysicalPredicate(pred));
            }
        }
        return preds;
    }

private:
    const JoinGraph& _joinGraph;
    const std::vector<ResolvedPath>& _resolvedPaths;
};

}  // namespace

std::unique_ptr<QuerySolution> constructSolutionWithRandomOrder(
    QuerySolutionMap solns,
    const JoinGraph& joinGraph,
    const std::vector<ResolvedPath>& resolvedPaths,
    int seed) {
    random_utils::PseudoRandomGenerator rand(seed);
    ReorderContext ctx(joinGraph, resolvedPaths);

    // Set of nodes we have already visited
    NodeSet visited;
    // Ordered queue of nodes we need to visit
    std::vector<NodeId> frontier;

    // Currently the stage builders only support the main collection being the left-most in the
    // tree.
    // TODO SERVER-111581: Remove this restriction.
    frontier.push_back(ctx.findMainCollectionNode());

    // Final query solution
    std::unique_ptr<QuerySolutionNode> soln;

    while (!frontier.empty()) {
        auto current = frontier.back();
        auto& currentNode = joinGraph.getNode(current);

        // Update solution to join the current node. For now always use a NLJ.
        if (!soln) {
            // TODO SERVER-111913: Avoid this clone
            soln = solns.at(currentNode.accessPath.get())->root()->clone();
        } else {
            // TODO SERVER-111913: Avoid this clone
            auto rhs = solns.at(currentNode.accessPath.get())->root()->clone();
            NodeSet currentNodeSet{};
            currentNodeSet.set(current);
            auto edges = joinGraph.getJoinEdges(visited, currentNodeSet);
            soln = std::make_unique<NestedLoopJoinEmbeddingNode>(std::move(soln),
                                                                 std::move(rhs),
                                                                 ctx.makeJoinPreds(edges),
                                                                 boost::none,
                                                                 currentNode.embedPath);
        }

        frontier.pop_back();
        visited.set(current);

        // Get unvisited neighbors
        auto neighbors = joinGraph.getNeighbors(current);
        std::vector<uint32_t> unvisited;
        for (size_t i = 0; i < neighbors.size(); ++i) {
            if (!neighbors.test(i)) {
                continue;
            }
            if (!visited.test(i)) {
                unvisited.push_back(i);
            }
        }
        // Randomize the order of the neighbors and add them to the queue
        rand.shuffleVector(unvisited);
        for (auto n : unvisited) {
            frontier.push_back(n);
        }
    }

    // TODO SERVER-111798: detect cycle and ensure that we apply all the join predicates.

    auto ret = std::make_unique<QuerySolution>();
    ret->setRoot(std::move(soln));
    return ret;
}

}  // namespace mongo::join_ordering
