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
#include "mongo/db/query/random_utils.h"

namespace mongo::join_ordering {

namespace {

/**
 * Represent sargable predicate that can be the RHS of an indexed nested loop join.
 */
struct IndexedJoinPredicate {
    QSNJoinPredicate::ComparisonOp op;
    FieldPath field;
};

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
        // Left field is a local field and potentially could come from already joined foreign
        // collection, so its embedPath is important to handle here. Right field is a foreign field
        // which comes from the current foreign collection, SBE does not expect it to be prefixed
        // with the foreign's collection as field.
        return {.op = op,
                .leftField = expandEmbeddedPath(pred.left),
                .rightField = _resolvedPaths[pred.right].fieldName};
    }

    std::vector<QSNJoinPredicate> makeJoinPreds(const JoinEdge& edge) {
        std::vector<QSNJoinPredicate> preds;
        preds.reserve(edge.predicates.size());
        for (auto pred : edge.predicates) {
            preds.push_back(makePhysicalPredicate(pred));
        }
        return preds;
    }

    std::vector<IndexedJoinPredicate> makeIndexedJoinPreds(const JoinEdge& edge,
                                                           NodeId currentNode) {
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

private:
    FieldPath expandEmbeddedPath(PathId pathId) {
        const auto& resolvedPath = _resolvedPaths[pathId];
        const auto& node = _joinGraph.getNode(_resolvedPaths[pathId].nodeId);
        if (node.embedPath.has_value()) {
            return node.embedPath->concat(resolvedPath.fieldName);
        }
        return resolvedPath.fieldName;
    }

    const JoinGraph& _joinGraph;
    const std::vector<ResolvedPath>& _resolvedPaths;
};

bool indexSatisfiesJoinPredicates(const IndexCatalogEntry& ice,
                                  std::vector<IndexedJoinPredicate>& joinPreds) {
    auto desc = ice.descriptor();
    if (desc->isHashedIdIndex() || desc->hidden() || desc->isPartial() || desc->isSparse() ||
        !desc->collation().isEmpty() || dynamic_cast<WildcardAccessMethod*>(ice.accessMethod())) {
        return false;
    }
    StringDataSet indexedPaths;
    for (auto&& elem : desc->keyPattern()) {
        indexedPaths.insert(elem.fieldNameStringData());
    }

    for (auto&& joinPred : joinPreds) {
        if (!indexedPaths.contains(joinPred.field.fullPath())) {
            return false;
        }
    }
    return true;
}

boost::optional<IndexEntry> indexSatisfyingJoinPredicates(
    const IndexCatalog& indexCatalog, std::vector<IndexedJoinPredicate>& joinPreds) {
    auto it = indexCatalog.getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
    while (it->more()) {
        auto ice = it->next();
        auto desc = ice->descriptor();
        // TODO SERVER-112939: Pick best available index to make this function deterministic in
        // prescense of multiple indexes which can serve as the probe side.
        if (indexSatisfiesJoinPredicates(*ice, joinPreds)) {
            return IndexEntry{desc->keyPattern(),
                              desc->getIndexType(),
                              desc->version(),
                              false /*isMultikey*/,
                              {} /*multikeyPaths*/,
                              {} /*multikeySet*/,
                              desc->isSparse(),
                              desc->unique(),
                              IndexEntry::Identifier{desc->indexName()},
                              ice->getFilterExpression(),
                              desc->infoObj(),
                              ice->getCollator(),
                              nullptr /*wildcardProjection*/,
                              ice->shared_from_this()};
        }
    }
    return boost::none;
}

}  // namespace

std::unique_ptr<QuerySolution> constructSolutionWithRandomOrder(
    QuerySolutionMap solns,
    const JoinGraph& joinGraph,
    const std::vector<ResolvedPath>& resolvedPaths,
    const MultipleCollectionAccessor& mca,
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

        // Update solution to join the current node.
        if (!soln) {
            // This is the first node we encountered.
            // TODO SERVER-111913: Avoid this clone
            soln = solns.at(currentNode.accessPath.get())->root()->clone();
        } else {
            // Generate an INLJ if possible, otherwise generate a NLJ.

            // TODO SERVER-111913: Avoid this clone
            auto rhs = solns.at(currentNode.accessPath.get())->root()->clone();
            NodeSet currentNodeSet{};
            currentNodeSet.set(current);
            auto edges = joinGraph.getJoinEdges(visited, currentNodeSet);

            // TODO SERVER-111798: Support join graphs with cycles
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
            if (edge.left.test(current)) {
                edge = edge.reverseEdge();
            }
            auto joinPreds = ctx.makeIndexedJoinPreds(edge, current);

            // Attempt to use INLJ if possible, otherwise fallback to NLJ.
            if (auto indexEntry = indexSatisfyingJoinPredicates(
                    *mca.lookupCollection(currentNode.collectionName)->getIndexCatalog(),
                    joinPreds);
                indexEntry.has_value()) {
                rhs = std::make_unique<FetchNode>(std::make_unique<IndexProbeNode>(
                    currentNode.collectionName, std::move(indexEntry.value())));
                soln = std::make_unique<IndexedNestedLoopJoinEmbeddingNode>(std::move(soln),
                                                                            std::move(rhs),
                                                                            ctx.makeJoinPreds(edge),
                                                                            boost::none,
                                                                            currentNode.embedPath);
            } else {
                soln = std::make_unique<NestedLoopJoinEmbeddingNode>(std::move(soln),
                                                                     std::move(rhs),
                                                                     ctx.makeJoinPreds(edge),
                                                                     boost::none,
                                                                     currentNode.embedPath);
            }
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
