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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/util/bitset_util.h"

namespace mongo::join_ordering {
namespace {
StringData toStringData(JoinPredicate::Operator op) {
    switch (op) {
        case JoinPredicate::Operator::Eq:
            return "eq";
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
        tassert(11233806,
                "only support swapping equality predicate",
                pred.op == JoinPredicate::Operator::Eq);
        std::swap(pred.left, pred.right);
    });
}
}  // namespace

std::string nodeSetToString(const NodeSet& set, size_t numNodesToPrint) {
    return set.to_string().substr(kHardMaxNodesInJoin - numNodesToPrint, numNodesToPrint);
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

    _nodes.emplace_back(std::move(collectionName), std::move(cq), std::move(embedPath));
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
}  // namespace mongo::join_ordering
