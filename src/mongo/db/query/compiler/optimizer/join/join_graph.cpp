// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/join/predicate_inferer.h"
#include "mongo/db/query/util/bitset_util.h"
#include "mongo/db/query/util/disjoint_set.h"

#include <string>
#include <string_view>

namespace mongo::join_ordering {
namespace {
std::string_view toStringData(JoinPredicate::Operator op) {
    switch (op) {
        case JoinPredicate::Operator::Eq:
            return "eq";
        case JoinPredicate::Operator::ExprEq:
            return "exprEq";
    }
    MONGO_UNREACHABLE_TASSERT(11147100);
}

BSONObj canonicalQueryToBSON(const std::unique_ptr<CanonicalQuery>& cq) {
    BSONObjBuilder accessPathBSON{};
    if (cq) {
        cq->serializeToBson(&accessPathBSON);
    }
    return accessPathBSON.obj();
}

static void swapPredicateSides(JoinEdge::PredicateList& predicates) {
    std::for_each(predicates.begin(), predicates.end(), [](JoinPredicate& pred) {
        tassert(11233806, "only support swapping equality predicate", pred.isEquality());
        std::swap(pred.left, pred.right);
    });
}
}  // namespace

std::string nodeSetToString(const NodeSet& set, size_t numNodesToPrint) {
    return set.to_string().substr(kHardMaxNodesInJoin - numNodesToPrint, numNodesToPrint);
}

std::vector<std::string> subsetCollectionNames(const NodeSet& set, const JoinGraph& graph) {
    std::vector<std::string> names;
    names.reserve(set.count());
    for (auto nodeIdx : iterable(set, graph.numNodes())) {
        names.emplace_back(
            redactTenant(graph.getNode(static_cast<NodeId>(nodeIdx)).collectionName));
    }
    return names;
}

BSONObj JoinNode::toBSON() const {
    BSONObjBuilder result{};
    result.append("collectionName", redactTenant(collectionName));
    result.append("accessPath", canonicalQueryToBSON(accessPath));
    result.append("embedPath", embedPath ? embedPath->fullPath() : "");
    return result.obj();
}

BSONObj JoinPredicate::toBSON() const {
    return BSON("op" << toStringData(op) << "left" << left << "right" << right);
}

BSONObj JoinEdge::toBSON() const {
    BSONObjBuilder result{};
    {
        BSONArrayBuilder ab{};
        for (const auto& pred : predicates) {
            ab.append(pred.toBSON());
        }
        result.append("predicates", ab.arr());
    }
    result.append("left", left);
    result.append("right", right);
    return result.obj();
}

JoinEdge JoinEdge::reverseEdge() const {
    JoinEdge ret{.left = right, .right = left};
    for (auto&& pred : predicates) {
        ret.predicates.push_back({.op = pred.op, .left = pred.right, .right = pred.left});
    }
    return ret;
}

void JoinEdge::insertPredicate(JoinPredicate pred) {
    if (pred.isEquality()) {
        // Check to see if we already have an equality predicate of a different type.
        JoinPredicate preExisting{.op = pred.op == JoinPredicate::Operator::Eq
                                      ? JoinPredicate::Operator::ExprEq
                                      : JoinPredicate::Operator::Eq,
                                  .left = pred.left,
                                  .right = pred.right};
        auto pos = std::find(predicates.begin(), predicates.end(), preExisting);
        if (pos != predicates.end()) {
            // Keep only the stricter version of equality ($expr).
            if (pos->op == JoinPredicate::Eq) {
                pos->op = JoinPredicate::ExprEq;
            }
            return;
        }
    }

    auto pos = std::find(predicates.begin(), predicates.end(), pred);
    if (pos == predicates.end()) {
        predicates.push_back(pred);
    }
}

boost::optional<NodeId> MutableJoinGraph::addNode(NamespaceString collectionName,
                                                  std::unique_ptr<CanonicalQuery> cq,
                                                  boost::optional<FieldPath> embedPath) {
    if (numNodes() >= _buildParams.maxNodesInJoin) {
        return boost::none;
    }

    std::unique_ptr<CanonicalQuery> originalFilter;
    if (cq) {
        // 'originalFilter' is a snapshot of the node's filter as parsed, before predicate
        // inference mutates 'accessPath' by ANDing in inferred single-table predicates.
        auto swOriginalFilter = cloneCQWithUpdatedFilter(
            cq->getExpCtx(), cq->nss(), cq->getPrimaryMatchExpression()->clone(), *cq);
        uassertStatusOK(swOriginalFilter.getStatus());
        originalFilter = std::move(swOriginalFilter.getValue());
    }

    _nodes.push_back(JoinNode{.collectionName = std::move(collectionName),
                              .originalFilter = std::move(originalFilter),
                              .accessPath = std::move(cq),
                              .embedPath = std::move(embedPath)});

    return static_cast<NodeId>(_nodes.size()) - 1;
}

boost::optional<EdgeId> MutableJoinGraph::addEdge(NodeId left,
                                                  NodeId right,
                                                  JoinEdge::PredicateList predicates) {
    // Self-edges are not permitted; when joining a collection to itself, we should use a different
    // node for each instance of the collection.
    tassert(11180001, "Self edges are not permitted", left != right);

    if (auto edgeId = _edgeMap.find(makeNodeSet(left, right)); edgeId != _edgeMap.end()) {
        return updateEdge(edgeId->second, left, std::move(predicates));
    }

    return makeEdge(left, right, std::move(predicates));
}

boost::optional<EdgeId> MutableJoinGraph::makeEdge(NodeId left,
                                                   NodeId right,
                                                   JoinEdge::PredicateList predicates) {
    if (_edges.size() >= _buildParams.maxEdgesInJoin ||
        _numberOfAddedPredicates + predicates.size() > _buildParams.maxPredicatesInJoin) {
        return boost::none;
    }

    if (right < left) {
        std::swap(left, right);
        swapPredicateSides(predicates);
    }

    NodeSet key = makeNodeSet(left, right);
    tassert(11116501, "The edge has been already added", !_edgeMap.contains(key));

    EdgeId edgeId = static_cast<EdgeId>(_edges.size());
    _edges.emplace_back(std::move(predicates), left, right);

    _edgeMap.emplace(key, edgeId);
    _numberOfAddedPredicates += _edges[edgeId].predicates.size();
    return edgeId;
}

boost::optional<EdgeId> MutableJoinGraph::updateEdge(EdgeId edgeId,
                                                     NodeId leftSideOfPredicates,
                                                     JoinEdge::PredicateList predicates) {
    if (_numberOfAddedPredicates + predicates.size() > _buildParams.maxPredicatesInJoin) {
        return boost::none;
    }

    auto&& edge = _edges[edgeId];
    if (edge.left != leftSideOfPredicates) {
        swapPredicateSides(predicates);
    }
    _numberOfAddedPredicates -= edge.predicates.size();
    edge.insertPredicates(predicates.begin(), predicates.end());
    _numberOfAddedPredicates += edge.predicates.size();
    return edgeId;
}

std::vector<EdgeId> JoinGraph::getJoinEdges(NodeSet left, NodeSet right) const {
    std::vector<EdgeId> result;

    // If left and right are alredy joined (have the same nodes) just return empty list of edges.
    if ((left & right).any()) {
        return result;
    }

    for (size_t edgeIndex = 0; edgeIndex < _edges.size(); ++edgeIndex) {
        const auto& edge = _edges[edgeIndex];
        if ((left[edge.left] && right[edge.right]) || (left[edge.right] && right[edge.left])) {
            result.push_back(static_cast<EdgeId>(edgeIndex));
        }
    }
    return result;
}

std::vector<EdgeId> JoinGraph::getEdgesForSubgraph(NodeSet nodes) const {
    std::vector<EdgeId> edges;
    if (nodes.count() <= 1) {
        // There are no self-edges.
        return edges;
    }
    for (const auto& [edgeBitset, edgeId] : _edgeMap) {
        // Subset check: all of this edge's bits are included in 'nodes'.
        if ((edgeBitset & nodes) == edgeBitset) {
            edges.push_back(edgeId);
        }
    }
    return edges;
}

NodeSet JoinGraph::getNeighbors(NodeId nodeIndex) const {
    NodeSet neighbors;
    for (const JoinEdge& edge : _edges) {
        if (edge.left == nodeIndex) {
            neighbors.set(edge.right);
        } else if (edge.right == nodeIndex) {
            neighbors.set(edge.left);
        }
    }
    return neighbors;
}

boost::optional<EdgeId> JoinGraph::findEdge(NodeId u, NodeId v) const {
    NodeSet key = makeNodeSet(u, v);
    auto pos = _edgeMap.find(key);
    if (pos == _edgeMap.end()) {
        return boost::none;
    }

    return pos->second;
}

BSONObj JoinGraph::toBSON() const {
    BSONObjBuilder result{};
    {
        BSONArrayBuilder ab{};
        for (const auto& node : _nodes) {
            ab.append(node.toBSON());
        }
        result.append("nodes", ab.arr());
    }
    {
        BSONArrayBuilder ab{};
        for (const auto& edge : _edges) {
            ab.append(edge.toBSON());
        }
        result.append("edges", ab.arr());
    }
    return result.obj();
}

JoinGraph::GraphShapeFlags JoinGraph::getShape() const {
    uint8_t shape = 0;
    if (!isConnected()) {
        return static_cast<GraphShapeFlags>(shape);
    }
    shape |= GraphShapeFlags::Connected;

    // A connected graph is a tree if it has no cycles, which is true if it has just enough edges to
    // connect all nodes, i.e. |edges| = |nodes| - 1.
    if (numEdges() == numNodes() - 1) {
        shape |= GraphShapeFlags::Tree;

        // We know the graph is a tree, so the helpers below don't need to check it again.
        if (isChain()) {
            shape |= GraphShapeFlags::Chain;
        }
        if (isStar()) {
            shape |= GraphShapeFlags::Star;
        }
    }

    // In an undirected graph clique, |edges| == |nodes| choose 2.
    if (numEdges() == (numNodes() * (numNodes() - 1) / 2)) {
        shape |= GraphShapeFlags::Clique;
    }

    // A connected graph is acyclic for: |nodes| > |edges|.
    if (numNodes() <= numEdges()) {
        shape |= GraphShapeFlags::Cycle;
    }

    return static_cast<GraphShapeFlags>(shape);
}

bool JoinGraph::isChain() const {
    // Note: this assumes each edge is between two nodes. In a chain query, every node appears twice
    // in the edge list, except for the end points. This means that after XOR'ing all the edge
    // NodeSets together, we see two bits set.
    NodeSet seen;
    for (auto&& [ns, edge] : _edgeMap) {
        seen ^= ns;
    }
    return seen.count() == 2;
}

bool JoinGraph::isStar() const {
    if (numEdges() < 2) {
        // Two-node graphs are trivially stars.
        return true;
    }

    auto edge0 = _edges[0];
    auto edge1 = _edges[1];
    NodeId center;
    if (edge0.left == edge1.left || edge0.left == edge1.right) {
        center = edge0.left;
    } else if (edge0.right == edge1.left || edge0.right == edge1.right) {
        center = edge0.right;
    } else {
        return false;
    }

    for (size_t i = 2; i < numEdges(); i++) {
        if (_edges[i].left != center && _edges[i].right != center) {
            return false;
        }
        // Otherwise, one of our edge end-points is the center! Continue.
    }
    return true;
}

bool JoinGraph::isConnected() const {
    if (_edges.size() < _nodes.size() - 1) {
        return false;
    }

    // We could implement the following with DFS. However, getting the neighbors of a node requires
    // iterating over all edges, which is inefficient. Instead, we use union-find.
    DisjointSet ds{_nodes.size()};
    for (const auto& edge : _edges) {
        ds.unite(edge.left, edge.right);
    }
    auto root = ds.find(0);
    for (size_t i = 1; i < _nodes.size(); ++i) {
        if (ds.find(i) != root) {
            return false;
        }
    }
    return true;
}

}  // namespace mongo::join_ordering
