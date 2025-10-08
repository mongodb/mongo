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
}  // namespace

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
    result.append("left", left.to_string());
    result.append("right", right.to_string());
    return result.obj();
}

std::vector<EdgeId> JoinGraph::getJoinEdges(NodeSet left, NodeSet right) const {
    std::vector<EdgeId> result;

    // If left and right are alredy joined (have the same nodes) just return empty list of edges.
    if ((left & right).any()) {
        return result;
    }

    for (size_t edgeIndex = 0; edgeIndex < _edges.size(); ++edgeIndex) {
        const auto& edge = _edges[edgeIndex];
        if (((left & edge.left).any() && (right & edge.right).any()) ||
            ((left & edge.right).any() && (right & edge.left).any())) {
            result.push_back(static_cast<EdgeId>(edgeIndex));
        }
    }
    return result;
}

NodeSet JoinGraph::getNeighbors(NodeId nodeIndex) const {
    NodeSet neighbors;
    for (const JoinEdge& edge : _edges) {
        if (edge.left.test(nodeIndex)) {
            neighbors |= edge.right;
        } else if (edge.right.test(nodeIndex)) {
            neighbors |= edge.left;
        }
    }
    return neighbors;
}

NodeId JoinGraph::addNode(NamespaceString collectionName,
                          std::unique_ptr<CanonicalQuery> cq,
                          boost::optional<FieldPath> embedPath) {
    _nodes.emplace_back(std::move(collectionName), std::move(cq), std::move(embedPath));
    return static_cast<NodeId>(_nodes.size()) - 1;
}

EdgeId JoinGraph::addEdge(NodeSet left, NodeSet right, JoinEdge::PredicateList predicates) {
    // Self-edges are not permitted; when joining a collection to itself, we should use a different
    // node for each instance of the collection.
    if (const auto common = (left & right); common.any()) {
        tasserted(11180001,
                  "Self edges are not permitted, but both sides contain " + common.to_string());
    }

    _edges.emplace_back(std::move(predicates), left, right);
    return static_cast<EdgeId>(_edges.size()) - 1;
}

EdgeId JoinGraph::addSimpleEqualityEdge(NodeId leftNode,
                                        NodeId rightNode,
                                        PathId leftPathId,
                                        PathId rightPathId) {

    NodeSet leftNodeSet{};
    leftNodeSet.set(leftNode);
    NodeSet rightNodeSet{};
    rightNodeSet.set(rightNode);
    return addEdge(leftNodeSet, rightNodeSet, {{JoinPredicate::Eq, leftPathId, rightPathId}});
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
