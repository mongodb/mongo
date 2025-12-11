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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/db/query/util/bitset_util.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/util/assert_util.h"

namespace mongo::join_ordering {

namespace {
/**
 * Helper function to simplify creation of BinaryJoinEmbedding QSNs.
 */
std::unique_ptr<QuerySolutionNode> makeBinaryJoinEmbeddingQSN(
    JoinMethod method,
    std::vector<QSNJoinPredicate>&& joinPreds,
    std::unique_ptr<QuerySolutionNode> leftChild,
    boost::optional<FieldPath> leftEmbedPath,
    std::unique_ptr<QuerySolutionNode> rightChild,
    boost::optional<FieldPath> rightEmbedPath) {
    switch (method) {
        case JoinMethod::HJ:
            return std::make_unique<HashJoinEmbeddingNode>(std::move(leftChild),
                                                           std::move(rightChild),
                                                           std::move(joinPreds),
                                                           leftEmbedPath,
                                                           rightEmbedPath);
        case JoinMethod::NLJ:
            return std::make_unique<NestedLoopJoinEmbeddingNode>(std::move(leftChild),
                                                                 std::move(rightChild),
                                                                 std::move(joinPreds),
                                                                 leftEmbedPath,
                                                                 rightEmbedPath);
        case JoinMethod::INLJ:
            return std::make_unique<IndexedNestedLoopJoinEmbeddingNode>(std::move(leftChild),
                                                                        std::move(rightChild),
                                                                        std::move(joinPreds),
                                                                        leftEmbedPath,
                                                                        rightEmbedPath);
    }
    MONGO_UNREACHABLE_TASSERT(11336909);
}

/**
 * Construct an appropriate IndexProbe for the given 'edge' & 'node', if possible.
 */
std::unique_ptr<QuerySolutionNode> createIndexProbeQSN(
    const JoinNode& node, std::shared_ptr<const IndexCatalogEntry> ice) {
    const auto& desc = ice->descriptor();
    std::unique_ptr<QuerySolutionNode> qsn = std::make_unique<FetchNode>(
        std::make_unique<IndexProbeNode>(node.collectionName,
                                         IndexEntry{desc->keyPattern(),
                                                    desc->getIndexType(),
                                                    desc->version(),
                                                    false /*isMultikey*/,
                                                    {} /*multikeyPaths*/,
                                                    {} /*multikeySet*/,
                                                    desc->isSparse(),
                                                    desc->unique(),
                                                    IndexEntry::Identifier{desc->indexName()},
                                                    desc->infoObj(),
                                                    nullptr /*wildcardProjection*/,
                                                    std::move(ice)}),
        node.collectionName);
    if (auto matchExpr = node.accessPath->getPrimaryMatchExpression();
        matchExpr != nullptr && !matchExpr->isTriviallyTrue()) {
        qsn->filter = matchExpr->clone();
    }
    return qsn;
}

/**
 * Given a PathId we need to add to a join predicate, we need to fetch its corresponding name
 * and (potentially) expand it to include the full path including where it is embedded.
 *
 * For example, consider the pipeline:
 *   [{$lookup: {from: "b", localField: "foo", foreignField: "bar", as: "b"}}, {$unwind: "$b"}].
 *
 * The field "bar" resolves to "bar" when not expanded and to "b.bar" when expanded.
 */
FieldPath expandEmbeddedPath(const JoinReorderingContext& ctx, PathId pathId, bool expand) {
    const auto& resolvedPath = ctx.resolvedPaths[pathId];
    if (!expand) {
        return resolvedPath.fieldName;
    }

    const auto& node = ctx.joinGraph.getNode(resolvedPath.nodeId);
    if (node.embedPath.has_value()) {
        return node.embedPath->concat(resolvedPath.fieldName);
    }
    return resolvedPath.fieldName;
}

QSNJoinPredicate makePhysicalPredicate(const JoinReorderingContext& ctx,
                                       JoinPredicate pred,
                                       bool expandLeftPath,
                                       bool expandRightPath) {
    QSNJoinPredicate::ComparisonOp op = [&pred] {
        switch (pred.op) {
            case JoinPredicate::Eq:
                return QSNJoinPredicate::ComparisonOp::Eq;
        }
        MONGO_UNREACHABLE_TASSERT(11075702);
    }();

    // Left field is a local field and potentially could come from already joined foreign
    // collection, so its embedPath is important to handle here. Right field is a foreign field
    // which comes from the current foreign collection, SBE does not expect it to be prefixed
    // with the foreign collection's as field.
    return {.op = op,
            .leftField = expandEmbeddedPath(ctx, pred.left, expandLeftPath),
            .rightField = expandEmbeddedPath(ctx, pred.right, expandRightPath)};
}

std::vector<QSNJoinPredicate> makeJoinPreds(const JoinReorderingContext& ctx,
                                            const JoinEdge& edge,
                                            bool expandLeftPath,
                                            bool expandRightPath) {
    std::vector<QSNJoinPredicate> preds;
    preds.reserve(edge.predicates.size());
    for (auto pred : edge.predicates) {
        preds.push_back(makePhysicalPredicate(ctx, pred, expandLeftPath, expandRightPath));
    }
    return preds;
}

// Forward-declare because of mutual recursion.
std::unique_ptr<QuerySolutionNode> buildQSNFromJoiningNode(const JoinReorderingContext& ctx,
                                                           const JoiningNode& join,
                                                           const JoinPlanNodeRegistry& registry);

std::unique_ptr<QuerySolutionNode> buildQSNFromJoinPlan(const JoinReorderingContext& ctx,
                                                        JoinPlanNodeId nodeId,
                                                        const JoinPlanNodeRegistry& registry) {
    std::unique_ptr<QuerySolutionNode> qsn;
    std::visit(OverloadedVisitor{[&ctx, &qsn, &registry](const JoiningNode& join) {
                                     qsn = buildQSNFromJoiningNode(ctx, join, registry);
                                 },
                                 [&qsn](const BaseNode& base) {
                                     // TODO SERVER-111913: Avoid this clone
                                     qsn = base.soln->root()->clone();
                                 }},
               registry.get(nodeId));
    return qsn;
}

NodeId getLeftmostNodeIdOfJoinPlan(const JoinReorderingContext& ctx,
                                   JoinPlanNodeId nodeId,
                                   const JoinPlanNodeRegistry& registry) {
    // Traverse binary tree to get left-most node, so we can use it as the base collection for
    // the aggregation itself- this is the first collection we join with.
    return std::visit(OverloadedVisitor{[&ctx, &registry](const JoiningNode& join) {
                                            return getLeftmostNodeIdOfJoinPlan(
                                                ctx, join.left, registry);
                                        },
                                        [](const BaseNode& base) {
                                            BitsetIterator<kMaxNodesInJoin> it = begin(base.bitset);
                                            return (NodeId)*it;
                                        }},
                      registry.get(nodeId));
}

JoinEdge getEdge(const JoinGraph& joinGraph, NodeSet left, NodeSet right) {
    auto edges = joinGraph.getJoinEdges(left, right);

    // TODO SERVER-111798: Support join graphs with cycles & multiple predicates.
    tassert(11233801,
            "expecting a single edge between visted set and current node. random reorderer "
            "does not support cycles yet",
            edges.size() == 1);
    auto edge = joinGraph.getEdge(edges[0]);

    // Ensure that edge is oriented the same way as the join (right side corresponds to
    // Index Probe side). This order is important for generating the 'QSNJoinPredicate'
    // which is order sensitive. Note that is a "cheating" a little bit because 'JoinEdge'
    // is logically an undirected edge in the graph but implemented with left/right ordered
    // members. We are exploiting this implementation detail to avoid doing duplicate work
    // of determining the orientation in making the 'IndexedJoinPredicate' and the
    // 'QSNJoinPredicate' below.
    if (right.test(edge.left)) {
        edge = edge.reverseEdge();
    }

    return edge;
}

const JoinNode& findFirstNode(const JoinGraph& joinGraph, NodeSet set) {
    for (size_t i = 0; i < kMaxNodesInJoin; i++) {
        if (set.test(i)) {
            return joinGraph.getNode((NodeId)i);
        }
    }
    MONGO_UNREACHABLE_TASSERT(11336910);
}

std::unique_ptr<QuerySolutionNode> buildQSNFromJoiningNode(const JoinReorderingContext& ctx,
                                                           const JoiningNode& join,
                                                           const JoinPlanNodeRegistry& registry) {
    auto leftChild = buildQSNFromJoinPlan(ctx, join.left, registry);
    auto rightChild = buildQSNFromJoinPlan(ctx, join.right, registry);

    const auto& leftSubset = registry.getBitset(join.left);
    const auto& rightSubset = registry.getBitset(join.right);

    // TODO SERVER-111798: Support join graphs with cycles & multiple predicates.
    auto edge = getEdge(ctx.joinGraph, leftSubset, rightSubset);

    const bool isLeftBaseNode = leftSubset.count() == 1;
    const bool isRightBaseNode = rightSubset.count() == 1;

    boost::optional<FieldPath> leftEmbedding, rightEmbedding;
    if (isLeftBaseNode) {
        // Node on the left may be embedded- we need to retrieve its embedding.
        const auto& leftNode = findFirstNode(ctx.joinGraph, leftSubset);
        leftEmbedding = leftNode.embedPath;
    }
    if (isRightBaseNode) {
        // Node on the right may be embedded- we need to retrieve its embedding.
        const auto& rightNode = findFirstNode(ctx.joinGraph, rightSubset);
        rightEmbedding = rightNode.embedPath;
    }

    // Only expand predicates for non-base nodes.
    bool expandLeftPath = !isLeftBaseNode;
    bool expandRightPath = !isRightBaseNode;
    auto joinPreds = makeJoinPreds(ctx, edge, expandLeftPath, expandRightPath);

    return makeBinaryJoinEmbeddingQSN(join.method,
                                      std::move(joinPreds),
                                      std::move(leftChild),
                                      leftEmbedding,
                                      std::move(rightChild),
                                      rightEmbedding);
}
}  // namespace

ReorderedJoinSolution constructSolutionWithRandomOrder(const JoinReorderingContext& ctx,
                                                       int seed,
                                                       bool defaultHJ) {
    random_utils::PseudoRandomGenerator rand(seed);

    // Set of nodes we have already visited
    NodeSet visited;
    // Ordered queue of nodes we need to visit
    std::vector<NodeId> frontier;

    // Randomly select a base collection.
    NodeId baseId = rand.generateUniformInt(0, (int)(ctx.joinGraph.numNodes() - 1));
    frontier.push_back(baseId);

    // Final query solution
    std::unique_ptr<QuerySolutionNode> soln;
    boost::optional<FieldPath> leftMostFieldPath;

    while (!frontier.empty()) {
        auto current = frontier.back();
        auto& currentNode = ctx.joinGraph.getNode(current);

        // Update solution to join the current node.
        if (!soln) {
            // This is the first node we encountered.
            // TODO SERVER-111913: Avoid this clone
            soln = ctx.cbrCqQsns.at(currentNode.accessPath.get())->root()->clone();
            leftMostFieldPath = currentNode.embedPath;
        } else {
            // Generate an INLJ if possible, otherwise generate a NLJ.

            // TODO SERVER-111913: Avoid this clone
            auto rhs = ctx.cbrCqQsns.at(currentNode.accessPath.get())->root()->clone();

            NodeSet currentNodeSet{};
            currentNodeSet.set(current);
            // TODO SERVER-111798: Support join graphs with cycles & multiple predicates.
            auto edge = getEdge(ctx.joinGraph, visited, currentNodeSet);

            boost::optional<FieldPath> lhsEmbedPath;
            bool expandLeftPredicate;
            if (auto lhs = dynamic_cast<BinaryJoinEmbeddingNode*>(soln.get()); !lhs) {
                // We are joining two "base" nodes in the tree and thus don't need to expand the
                // field referenced in the join predicate.
                lhsEmbedPath = leftMostFieldPath;
                // In this case, we don't want to expand either field in the predicate! Instead, we
                // want to use the SBE slot for both fields.
                expandLeftPredicate = false;
            } else {
                // Our rhs is an access path node, and our lhs is some embedding node tree. We want
                // to expand only the path corresponding to the left subtree in the predicate;
                // however, note that the order of paths may not match the order of nodes.
                expandLeftPredicate = true;
            }

            auto joinPreds = makeJoinPreds(
                ctx, edge, expandLeftPredicate, false /* Left-deep => never expand RHS. */);

            // Attempt to use INLJ if possible, otherwise fallback to NLJ or HJ depending on the
            // query knob.
            JoinMethod method = JoinMethod::NLJ;
            if (auto ice = bestIndexSatisfyingJoinPredicates(ctx, current, edge); ice) {
                rhs = createIndexProbeQSN(currentNode, ice);
                method = JoinMethod::INLJ;
            } else if (defaultHJ) {
                method = JoinMethod::HJ;
            }

            soln = makeBinaryJoinEmbeddingQSN(method,
                                              std::move(joinPreds),
                                              std::move(soln),
                                              lhsEmbedPath,
                                              std::move(rhs),
                                              currentNode.embedPath);
        }

        frontier.pop_back();
        visited.set(current);

        // Get unvisited neighbors
        auto neighbors = ctx.joinGraph.getNeighbors(current);
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
    return {.soln = std::move(ret), .baseNode = baseId};
}

ReorderedJoinSolution constructSolutionBottomUp(const JoinReorderingContext& ctx,
                                                PlanTreeShape shape) {
    PlanEnumeratorContext peCtx(ctx);

    peCtx.enumerateJoinSubsets(shape);
    auto bestPlanNodeId = peCtx.getBestFinalPlan();

    // Build QSN based on best plan.
    auto ret = std::make_unique<QuerySolution>();
    const auto& registry = peCtx.registry();
    auto baseNodeId = getLeftmostNodeIdOfJoinPlan(ctx, bestPlanNodeId, registry);
    ret->setRoot(buildQSNFromJoinPlan(ctx, bestPlanNodeId, registry));
    return {.soln = std::move(ret), .baseNode = baseNodeId};
}

}  // namespace mongo::join_ordering
