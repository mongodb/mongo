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
#include "mongo/util/modules.h"

#include <algorithm>

/** This file introduces the join optimizer's logical model. It defines classes representing a join
 * graph and its components.
 */
namespace mongo::join_ordering {
/** The maximum number of nodes which can participate in one join.
 */
constexpr size_t kMaxNodesInJoin = 64;

/** The maximum number of edges in a Join Graph.
 */
constexpr size_t kMaxEdgesInJoin = std::numeric_limits<EdgeId>::max();

/** NodeSet is a bitset representation of a subset of join nodes. It is used to efficiently
 * track which nodes are included in an intermediate join. This compact representation is highly
 * effective for the join reordering algorithm.
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

    friend auto operator<=>(const JoinPredicate& lhs, const JoinPredicate& rhs) = default;
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

    /**
     * Insert in a new predicate to the edge, it is no-op if the predicate already is presented.
     * The function assumes that the predicate has the same orientation (left/right side) as the
     * edge.
     */
    void insertPredicate(JoinPredicate pred);

    /**
     * Insert the predicates from the range ['first', 'last'), which are not yet presented in the
     * edge. The function assumes that the predicates have the same orientation (left/right side) as
     * the edge.
     */
    template <typename It>
    void insertPredicates(It first, It last) {
        std::for_each(first, last, [this](JoinPredicate pred) { insertPredicate(pred); });
    }
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

    /**
     * Adds a new node. Returns the id of the new node or boost::none if the maximum number of join
     * nodes has been reached.
     */
    boost::optional<NodeId> addNode(NamespaceString collectionName,
                                    std::unique_ptr<CanonicalQuery> cq,
                                    boost::optional<FieldPath> embedPath);

    /**
     * Adds a new edge or add predicates if the edge with the specified 'left' and 'right' exists.
     * Returns the id of the edge or boost::none if the maximum number of join edges has been
     * reached.
     */
    boost::optional<EdgeId> addEdge(NodeSet left,
                                    NodeSet right,
                                    JoinEdge::PredicateList predicates);

    /**
     * Adds a new edge or add predicates if the edge with the specified 'left' and 'right' exists.
     * Returns the id of the edge or boost::none if the maximum number of join edges has been
     * reached.
     */
    boost::optional<EdgeId> addSimpleEqualityEdge(NodeId leftNode,
                                                  NodeId rightNode,
                                                  PathId leftPathId,
                                                  PathId rightPathId);

    /**
     * Returns EdgeId of the edge that connects u and v. This check is order-independent, meaning
     * the returned edge might be (u, v) or (v, u).
     */
    boost::optional<EdgeId> findEdge(NodeSet u, NodeSet v) const;

    boost::optional<EdgeId> findSimpleEdge(NodeId u, NodeId v) const {
        NodeSet uns{};
        uns.set(u);
        NodeSet vns{};
        vns.set(v);
        return findEdge(uns, vns);
    }

    const JoinNode& getNode(NodeId nodeId) const {
        if constexpr (kDebugBuild) {
            return _nodes.at(nodeId);
        } else {
            return _nodes[nodeId];
        }
    }

    const CanonicalQuery* accessPathAt(NodeId nodeId) const {
        return getNode(nodeId).accessPath.get();
    }

    const JoinEdge& getEdge(EdgeId edgeId) const {
        if constexpr (kDebugBuild) {
            return _edges.at(edgeId);
        } else {
            return _edges[edgeId];
        }
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

    const std::vector<JoinEdge>& edges() const {
        return _edges;
    }

private:
    /**
     * Creates a new edge with the specified 'left' and 'right' nodesets and 'predicates'. It's the
     * only correct way to create edges and must not be called for an existing edge, since it
     * maintains the invariant that only a single edge exists between any two node sets containing
     * the conjunction of all predicates.
     */
    boost::optional<EdgeId> makeEdge(NodeSet left,
                                     NodeSet right,
                                     JoinEdge::PredicateList predicates);

    std::vector<JoinNode> _nodes;
    std::vector<JoinEdge> _edges;
    // Maps a pair of nodesets to the edge that connects them, The node sets are stored
    // in a key in the way that the first node is smaller than the second one.
    absl::flat_hash_map<std::pair<NodeSet, NodeSet>, EdgeId> _edgeMap;
};

}  // namespace mongo::join_ordering
