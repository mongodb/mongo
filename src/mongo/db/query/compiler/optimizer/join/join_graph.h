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

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"

/** This file introduces the join optimizer's logical model. It defines classes representing a join
 * graph and its components.
 */
namespace mongo::join_ordering {
/** The maximum number of nodes which can participate in one join.
 */
constexpr uint32_t kMaxNodesInJoin = 64;

/** NodeSet is a bitset representation of a subset of join nodes. It is used to efficiently track
 * which nodes are included in an intermediate join. This compact representation is highly effective
 * for the join reordering algorithm.
 */
using NodeSet = std::bitset<kMaxNodesInJoin>;

/** JoinNode represents a single occurrence of a collection in a query. A new join node is created
for each time a collection appears. This ensures every instance is uniquely identified within the
join graph.
 */
struct JoinNode {
    NamespaceString collectionName;

    std::unique_ptr<mongo::CanonicalQuery> accessPath;

    /* Prefix path for the collection's fields. This path indicates where the field will be stored
     * in a new document. For the main collection, this path is always empty.
     */
    boost::optional<FieldPath> embedPath;

    /** Serializes the Join Node to BSON.
     */
    BSONObj toBSON() const;
};

/** A join predicate is a condition that specifies how two collections should be joined. It has two
 * fields, one from each of the joining collections. At the momemnt the only operator used is
 * equality, which creates an equi-join. This predicate links documents from the two collections
 * where the values of the specified fields are equal. For example, joining a customers collection
 * and an orders collection might use the predicate customers.id = orders.customer_id.
 */
struct JoinPredicate {
    enum Operator {
        Eq,
    };

    Operator op;
    PathId left;
    PathId right;

    /** Serializes the Join Predicate to BSON.
     */
    BSONObj toBSON() const;
};

/**
 * Represents an undirected join edge between two sets of collections.
 *
 * Only one-to-one connections are currently supported. To prepare for future many-to-many join edge
 * support, the left and right sides are defined as NodeSets.
 */
struct JoinEdge {
    using PredicateList = absl::InlinedVector<JoinPredicate, 2>;

    PredicateList predicates;

    NodeSet left;
    NodeSet right;

    NodeSet getBitset() const {
        return left | right;
    }

    /** Serializes the Join Edge to BSON.
     */
    BSONObj toBSON() const;

    /**
     * Return a new edge with left/right node sets and predicates swapped. This is useful for code
     * which relies on the order of the predicates of the edge, despite the edge being undirected.
     */
    JoinEdge reverseEdge() const;
};

/** A join graph is a logical model that represents the joins in a query. It consists of join nodes
 * and join edges. The nodes represent the collections being queried, and the edges represent the
 * predicates that connect them.
 */
class JoinGraph {
public:
    /** Return the list of edges which can merge two intermediate joins.
     */
    std::vector<EdgeId> getJoinEdges(NodeSet left, NodeSet right) const;

    /** Get neighbors of the given node.
     */
    NodeSet getNeighbors(NodeId nodeIndex) const;

    NodeId addNode(NamespaceString collectionName,
                   std::unique_ptr<CanonicalQuery> cq,
                   boost::optional<FieldPath> embedPath);

    EdgeId addEdge(NodeSet left, NodeSet right, JoinEdge::PredicateList predicates);

    EdgeId addSimpleEqualityEdge(NodeId leftNode,
                                 NodeId rightNode,
                                 PathId leftPathId,
                                 PathId rightPathId);

    const JoinNode& getNode(NodeId nodeId) const {
        return _nodes[nodeId];
    }

    const JoinEdge& getEdge(EdgeId edgeId) const {
        return _edges[edgeId];
    }

    size_t numNodes() const {
        return _nodes.size();
    }

    size_t numEdges() const {
        return _edges.size();
    }

    /** Serializes the Join Graph to BSON.
     */
    BSONObj toBSON() const;

    /** Converts the Join Graph to a JSON string. If 'pretty' is true the output JSON string is
     * idented.
     */
    std::string toString(bool pretty) const {
        return toBSON().jsonString(/*format*/ ExtendedCanonicalV2_0_0, pretty);
    }

private:
    std::vector<JoinNode> _nodes;
    std::vector<JoinEdge> _edges;
};

}  // namespace mongo::join_ordering
