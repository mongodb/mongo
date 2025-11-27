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

#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/plan_enumerator.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/db/query/util/bitset_iterator.h"
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

class ReorderContext {
public:
    ReorderContext(const JoinGraph& joinGraph, const std::vector<ResolvedPath>& resolvedPaths)
        : _joinGraph(joinGraph), _resolvedPaths(resolvedPaths) {}

    ReorderContext(const ReorderContext&) = delete;
    ReorderContext(ReorderContext&&) = delete;
    ReorderContext& operator=(const ReorderContext&) = delete;
    ReorderContext& operator=(ReorderContext&&) = delete;

    QSNJoinPredicate makePhysicalPredicate(JoinPredicate pred,
                                           bool expandLeftPath,
                                           bool expandRightPath) const {
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
                .leftField = expandEmbeddedPath(pred.left, expandLeftPath),
                .rightField = expandEmbeddedPath(pred.right, expandRightPath)};
    }

    std::vector<QSNJoinPredicate> makeJoinPreds(const JoinEdge& edge,
                                                bool expandLeftPath,
                                                bool expandRightPath) const {
        std::vector<QSNJoinPredicate> preds;
        preds.reserve(edge.predicates.size());
        for (auto pred : edge.predicates) {
            preds.push_back(makePhysicalPredicate(pred, expandLeftPath, expandRightPath));
        }
        return preds;
    }

    std::vector<IndexedJoinPredicate> makeIndexedJoinPreds(const JoinEdge& edge,
                                                           NodeId currentNode) const {
        tassert(11233804, "left edge expected only one node", edge.left.count() == 1);
        tassert(11233805, "right edge expected only one node", edge.right.count() == 1);
        std::vector<IndexedJoinPredicate> res;
        for (auto&& pred : edge.predicates) {
            res.push_back({
                .op = QSNJoinPredicate::ComparisonOp::Eq,
                .field = _resolvedPaths[pred.right].fieldName,
            });
        }
        return res;
    }

    std::unique_ptr<QuerySolutionNode> buildQSNFromJoinPlan(
        JoinPlanNodeId nodeId, const JoinPlanNodeRegistry& registry) const {
        std::unique_ptr<QuerySolutionNode> qsn;
        std::visit(OverloadedVisitor{[this, &qsn, &registry](const JoiningNode& join) {
                                         qsn = buildQSNFromJoiningNode(join, registry);
                                     },
                                     [&qsn](const BaseNode& base) {
                                         // TODO SERVER-111913: Avoid this clone
                                         qsn = base.soln->root()->clone();
                                     }},
                   registry.get(nodeId));
        return qsn;
    }

    NodeId getLeftmostNodeIdOfJoinPlan(JoinPlanNodeId nodeId,
                                       const JoinPlanNodeRegistry& registry) const {
        // Traverse binary tree to get left-most node, so we can use it as the base collection for
        // the aggregation itself- this is the first collection we join with.
        return std::visit(
            OverloadedVisitor{[this, &registry](const JoiningNode& join) {
                                  return getLeftmostNodeIdOfJoinPlan(join.left, registry);
                              },
                              [](const BaseNode& base) {
                                  BitsetIterator<kMaxNodesInJoin> it = begin(base.bitset);
                                  return (NodeId)*it;
                              }},
            registry.get(nodeId));
    }

    JoinEdge getEdge(NodeSet left, NodeSet right) const {
        auto edges = _joinGraph.getJoinEdges(left, right);

        // TODO SERVER-111798: Support join graphs with cycles & multiple predicates.
        tassert(11233801,
                "expecting a single edge between visted set and current node. random reorderer "
                "does not support cycles yet",
                edges.size() == 1);
        auto edge = _joinGraph.getEdge(edges[0]);

        // Ensure that edge is oriented the same way as the join (right side corresponds to
        // Index Probe side). This order is important for generating the 'QSNJoinPredicate'
        // which is order sensitive. Note that is a "cheating" a little bit because 'JoinEdge'
        // is logically an undirected edge in the graph but implemented with left/right ordered
        // members. We are exploiting this implementation detail to avoid doing duplicate work
        // of determining the orientation in making the 'IndexedJoinPredicate' and the
        // 'QSNJoinPredicate' below.
        if ((edge.left & right).any()) {
            edge = edge.reverseEdge();
        }

        return edge;
    }

private:
    /**
     * Given a PathId we need to add to a join predicate, we need to fetch its corresponding name
     * and (potentially) expand it to include the full path including where it is embedded.
     *
     * For example, consider the pipeline:
     *   [{$lookup: {from: "b", localField: "foo", foreignField: "bar", as: "b"}}, {$unwind: "$b"}].
     *
     * The field "bar" resolves to "bar" when not expanded and to "b.bar" when expanded.
     */
    FieldPath expandEmbeddedPath(PathId pathId, bool expand) const {
        const auto& resolvedPath = _resolvedPaths[pathId];
        if (!expand) {
            return resolvedPath.fieldName;
        }

        const auto& node = _joinGraph.getNode(resolvedPath.nodeId);
        if (node.embedPath.has_value()) {
            return node.embedPath->concat(resolvedPath.fieldName);
        }
        return resolvedPath.fieldName;
    }

    const JoinNode& findFirstNode(NodeSet set) const {
        for (size_t i = 0; i < kMaxNodesInJoin; i++) {
            if (set.test(i)) {
                return _joinGraph.getNode((NodeId)i);
            }
        }
        MONGO_UNREACHABLE_TASSERT(11336910);
    }

    std::unique_ptr<QuerySolutionNode> buildQSNFromJoiningNode(
        const JoiningNode& join, const JoinPlanNodeRegistry& registry) const {
        auto leftChild = buildQSNFromJoinPlan(join.left, registry);
        auto rightChild = buildQSNFromJoinPlan(join.right, registry);

        const auto& leftSubset = registry.getBitset(join.left);
        const auto& rightSubset = registry.getBitset(join.right);

        // TODO SERVER-111798: Support join graphs with cycles & multiple predicates.
        auto edge = getEdge(leftSubset, rightSubset);

        const bool isLeftBaseNode = leftSubset.count() == 1;
        const bool isRightBaseNode = rightSubset.count() == 1;

        boost::optional<FieldPath> leftEmbedding, rightEmbedding;
        if (isLeftBaseNode) {
            // Node on the left may be embedded- we need to retrieve its embedding.
            const auto& leftNode = findFirstNode(leftSubset);
            leftEmbedding = leftNode.embedPath;
        }
        if (isRightBaseNode) {
            // Node on the right may be embedded- we need to retrieve its embedding.
            const auto& rightNode = findFirstNode(rightSubset);
            rightEmbedding = rightNode.embedPath;
        }

        // Only expand predicates for non-base nodes.
        bool expandLeftPath = !isLeftBaseNode;
        bool expandRightPath = !isRightBaseNode;
        auto joinPreds = makeJoinPreds(edge, expandLeftPath, expandRightPath);

        return makeBinaryJoinEmbeddingQSN(join.method,
                                          std::move(joinPreds),
                                          std::move(leftChild),
                                          leftEmbedding,
                                          std::move(rightChild),
                                          rightEmbedding);
    }

    const JoinGraph& _joinGraph;
    const std::vector<ResolvedPath>& _resolvedPaths;
};

std::shared_ptr<const IndexCatalogEntry> betterIndexForProbe(
    std::shared_ptr<const IndexCatalogEntry> first,
    std::shared_ptr<const IndexCatalogEntry> second) {
    auto& firstKeyPattern = first->descriptor()->keyPattern();
    auto& secondKeyPattern = second->descriptor()->keyPattern();
    if (firstKeyPattern.nFields() < secondKeyPattern.nFields()) {
        return first;
    } else if (firstKeyPattern.nFields() > secondKeyPattern.nFields()) {
        return second;
    }
    if (firstKeyPattern.woCompare(secondKeyPattern) > 0) {
        return second;
    }
    return first;
}

}  // namespace

bool indexSatisfiesJoinPredicates(const IndexCatalogEntry& ice,
                                  const std::vector<IndexedJoinPredicate>& joinPreds) {
    auto desc = ice.descriptor();
    if (desc->isHashedIdIndex() || desc->hidden() || desc->isPartial() || desc->isSparse() ||
        !desc->collation().isEmpty() || dynamic_cast<WildcardAccessMethod*>(ice.accessMethod())) {
        return false;
    }
    StringSet joinFields;
    for (auto&& joinPred : joinPreds) {
        joinFields.insert(joinPred.field.fullPath());
    }
    for (auto&& elem : desc->keyPattern()) {
        auto it = joinFields.find(elem.fieldName());
        if (it != joinFields.end()) {
            joinFields.erase(it);
        } else {
            break;
        }
    }
    return joinFields.empty();
}

boost::optional<IndexEntry> bestIndexSatisfyingJoinPredicates(
    const IndexCatalog& indexCatalog, const std::vector<IndexedJoinPredicate>& joinPreds) {
    auto it = indexCatalog.getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    std::shared_ptr<const IndexCatalogEntry> bestIndex = nullptr;
    for (auto&& ice : indexCatalog.getEntriesShared(IndexCatalog::InclusionPolicy::kReady)) {
        if (indexSatisfiesJoinPredicates(*ice, joinPreds)) {
            if (!bestIndex) {
                bestIndex = ice;
            } else {
                // Keep the better suited index in 'bestIndex'.
                bestIndex = betterIndexForProbe(bestIndex, ice);
            }
        }
    }
    if (bestIndex) {
        auto desc = bestIndex->descriptor();
        auto filterExpression = bestIndex->getFilterExpression();
        auto collator = bestIndex->getCollator();
        return IndexEntry{desc->keyPattern(),
                          desc->getIndexType(),
                          desc->version(),
                          false /*isMultikey*/,
                          {} /*multikeyPaths*/,
                          {} /*multikeySet*/,
                          desc->isSparse(),
                          desc->unique(),
                          IndexEntry::Identifier{desc->indexName()},
                          filterExpression,
                          desc->infoObj(),
                          collator,
                          nullptr /*wildcardProjection*/,
                          std::move(bestIndex)};
    }
    return boost::none;
}

ReorderedJoinSolution constructSolutionWithRandomOrder(
    QuerySolutionMap solns,
    const JoinGraph& joinGraph,
    const std::vector<ResolvedPath>& resolvedPaths,
    const MultipleCollectionAccessor& mca,
    int seed,
    bool defaultHJ,
    bool enableBaseReordering) {
    random_utils::PseudoRandomGenerator rand(seed);
    ReorderContext ctx(joinGraph, resolvedPaths);

    // Set of nodes we have already visited
    NodeSet visited;
    // Ordered queue of nodes we need to visit
    std::vector<NodeId> frontier;

    // Randomly select a base collection.
    NodeId baseId = enableBaseReordering
        ? rand.generateUniformInt(0, (int)(joinGraph.numNodes() - 1))
        : 0 /* Use first node seen by graph. */;
    frontier.push_back(baseId);

    // Final query solution
    std::unique_ptr<QuerySolutionNode> soln;
    boost::optional<FieldPath> leftMostFieldPath;

    while (!frontier.empty()) {
        auto current = frontier.back();
        auto& currentNode = joinGraph.getNode(current);

        // Update solution to join the current node.
        if (!soln) {
            // This is the first node we encountered.
            // TODO SERVER-111913: Avoid this clone
            soln = solns.at(currentNode.accessPath.get())->root()->clone();
            leftMostFieldPath = currentNode.embedPath;
        } else {
            // Generate an INLJ if possible, otherwise generate a NLJ.

            // TODO SERVER-111913: Avoid this clone
            auto rhs = solns.at(currentNode.accessPath.get())->root()->clone();

            NodeSet currentNodeSet{};
            currentNodeSet.set(current);
            // TODO SERVER-111798: Support join graphs with cycles & multiple predicates.
            auto edge = ctx.getEdge(visited, currentNodeSet);

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

            auto joinPreds = ctx.makeJoinPreds(
                edge, expandLeftPredicate, false /* Left-deep => never expand RHS. */);

            // Attempt to use INLJ if possible, otherwise fallback to NLJ or HJ depending on the
            // query knob.
            JoinMethod method = JoinMethod::NLJ;
            auto indexedJoinPreds = ctx.makeIndexedJoinPreds(edge, current);
            if (auto indexEntry = bestIndexSatisfyingJoinPredicates(
                    *mca.lookupCollection(currentNode.collectionName)->getIndexCatalog(),
                    indexedJoinPreds);
                indexEntry.has_value()) {
                rhs = std::make_unique<FetchNode>(
                    std::make_unique<IndexProbeNode>(currentNode.collectionName,
                                                     std::move(indexEntry.value())),
                    currentNode.collectionName);
                // TODO SERVER-111222: Write an end-to-end test exercising this codepath, once we
                // can lower INLJ nodes to SBE.
                if (auto matchExpr = currentNode.accessPath->getPrimaryMatchExpression();
                    matchExpr != nullptr && !matchExpr->isTriviallyTrue()) {
                    rhs->filter = matchExpr->clone();
                }
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
    return {.soln = std::move(ret), .baseNode = baseId};
}

ReorderedJoinSolution constructSolutionBottomUp(QuerySolutionMap solns,
                                                const join_ordering::JoinGraph& joinGraph,
                                                const std::vector<ResolvedPath>& resolvedPaths,
                                                const MultipleCollectionAccessor& catalog) {
    PlanEnumeratorContext peCtx(joinGraph, solns);
    ReorderContext rCtx(joinGraph, resolvedPaths);

    peCtx.enumerateJoinSubsets(PlanTreeShape::LEFT_DEEP);
    auto bestPlanNodeId = peCtx.getBestFinalPlan();

    // Build QSN based on best plan.
    auto ret = std::make_unique<QuerySolution>();
    const auto& registry = peCtx.registry();
    auto baseNodeId = rCtx.getLeftmostNodeIdOfJoinPlan(bestPlanNodeId, registry);
    ret->setRoot(rCtx.buildQSNFromJoinPlan(bestPlanNodeId, registry));
    return {.soln = std::move(ret), .baseNode = baseNodeId};
}

}  // namespace mongo::join_ordering
