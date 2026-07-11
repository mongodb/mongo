// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/optimizer/join/join_estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_method.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/util/bitset_util.h"
#include "mongo/util/modules.h"

#include <variant>

namespace mongo::join_ordering {

// Forward declarations.
struct JoiningNode;
struct BaseNode;
struct INLJRHSNode;

using JoinPlanNodeId = size_t;
using JoinPlanNode = std::variant<JoiningNode, BaseNode, INLJRHSNode>;
using JoinPlans = std::vector<JoinPlanNodeId>;

/**
 * Represents a subset of a JoinGraph, and tracks plans enumerated so far for this subset, as well
 * as the best plan we have enumerated so far.
 */
struct JoinSubset {
    explicit JoinSubset(NodeSet set) : subset(set), bestPlanIndex(0) {}

    // Delete copy operations.
    JoinSubset(const JoinSubset&) = delete;
    JoinSubset& operator=(const JoinSubset&) = delete;

    // Need move operations to instantiate & resize vectors of JoinSubsets.
    JoinSubset(JoinSubset&& other) = default;
    JoinSubset& operator=(JoinSubset&& other) = default;

    inline bool isBaseCollectionAccess() const {
        return subset.count() == 1;
    }

    NodeId getNodeId() const {
        tassert(11371703, "Must have just one node", isBaseCollectionAccess());
        return (NodeId)*begin(subset);
    }

    inline bool hasPlans() const {
        return !plans.empty();
    }

    inline JoinPlanNodeId bestPlan() const {
        tassert(11566600, "Failed to find a plan", hasPlans());
        tassert(11336908, "Expected bestPlanIndex < plans.size()", bestPlanIndex < plans.size());
        return plans[bestPlanIndex];
    }

    // Bitset representing which JoinGraph nodes are present in this subset.
    NodeSet subset;

    // JoinPlans that could implement this JoinSubset.
    JoinPlans plans;

    // Index of the lowest-cost plan in 'plans'.
    size_t bestPlanIndex;

    std::string toString(size_t numNodesToPrint = kHardMaxNodesInJoin) const;
};

/**
 * A JoinPlan node representing a base collection access.
 */
struct BaseNode {
    // Pointer to best access path obtained by CBR.
    const QuerySolution* soln;

    // Namespace this access path corresponds to.
    const NamespaceString& nss;

    // Corresponds to node in the graph this represents a base table access to.
    const NodeId node;

    // Cost of base collection access.
    JoinCostEstimate cost;

    std::string toString(size_t numNodesToPrint = kHardMaxNodesInJoin,
                         std::string indentStr = "") const;
};

/**
 * A JoinPlan node representing the right hand side of a INLJ join (an index probe).
 */
struct INLJRHSNode {
    // The index that will be used to construct an IndexProbe QSN.
    std::shared_ptr<const IndexCatalogEntry> entry;

    // Namespace this access path corresponds to.
    const NamespaceString& nss;

    // Corresponds to node in the graph this represents a base table access to.
    const NodeId node;

    std::string toString(size_t numNodesToPrint = kHardMaxNodesInJoin,
                         std::string indentStr = "") const;
};

struct JoiningNode {
    const JoinMethod method;

    // Children. Note that this node does not manage the memory for its children- see
    // 'JoinPlanNodeRegistry'. The registry can be used to retrieve each child by id.
    const JoinPlanNodeId left;
    const JoinPlanNodeId right;

    // Keeps a copy of the bitset representing the subgraph this originated from.
    const NodeSet bitset;

    // Cost of join.
    JoinCostEstimate cost;

    std::string toString(size_t numNodesToPrint = kHardMaxNodesInJoin,
                         std::string indentStr = "") const;
};

std::ostream& operator<<(std::ostream& os, const JoinSubset& subset);

/**
 * Get the cost estimate of the given 'JoinPlanNode'. This operation is only defined for 'BaseNode'
 * and 'JoiningNode'.
 */
inline JoinCostEstimate getNodeCost(const JoinPlanNode& node) {
    return std::visit(OverloadedVisitor{[](const JoiningNode& join) { return join.cost; },
                                        [](const INLJRHSNode& ip) {
                                            // These nodes don't have their own cost.
                                            MONGO_UNREACHABLE_TASSERT(11727800);
                                            return JoinCostEstimate(zeroCE, zeroCE, zeroCE, zeroCE);
                                        },
                                        [](const BaseNode& base) {
                                            return base.cost;
                                        }},
                      node);
}

/**
 * Get the node subset which this given 'JoinPlanNode' contains the solution for.
 */
inline NodeSet getNodeBitset(const JoinPlanNode& node) {
    return std::visit(
        OverloadedVisitor{[](const JoiningNode& join) { return join.bitset; },
                          [](const INLJRHSNode& ip) { return NodeSet().set(ip.node); },
                          [](const BaseNode& base) {
                              return NodeSet().set(base.node);
                          }},
        node);
}

/**
 * We don't want to reallocate memory for every node we reuse for some other subtree. This means
 * that every node could have multiple parents. In order to make sure we manage memory sanely and
 * efficiently, we delegate responsibility for managing node pointers to this registry, which
 * functions as a node Arena. It outputs JoinPlanNodeIds, and retrieves JoinPlanNodes by id.
 */
class JoinPlanNodeRegistry {
public:
    JoinPlanNodeRegistry() = default;

    // Delete copy and move operations to prevent issues with copying '_allJoinPlans'.
    JoinPlanNodeRegistry(const JoinPlanNodeRegistry&) = delete;
    JoinPlanNodeRegistry(JoinPlanNodeRegistry&&) = delete;
    JoinPlanNodeRegistry& operator=(const JoinPlanNodeRegistry&) = delete;
    JoinPlanNodeRegistry& operator=(JoinPlanNodeRegistry&&) = delete;

    JoinPlanNodeId registerJoinNode(const JoinSubset& subset,
                                    JoinMethod method,
                                    JoinPlanNodeId left,
                                    JoinPlanNodeId right,
                                    JoinCostEstimate cost);
    JoinPlanNodeId registerBaseNode(NodeId node,
                                    const QuerySolution* soln,
                                    const NamespaceString& nss,
                                    JoinCostEstimate cost);
    JoinPlanNodeId registerINLJRHSNode(NodeId node,
                                       std::shared_ptr<const IndexCatalogEntry> entry,
                                       const NamespaceString& nss);

    const JoinPlanNode& get(JoinPlanNodeId id) const {
        return _allJoinPlans[id];
    }

    NodeSet getBitset(JoinPlanNodeId id) const;
    // Note: not all node types have an associated cost.
    JoinCostEstimate getCost(JoinPlanNodeId id) const;

    template <typename T>
    const T& getAs(JoinPlanNodeId id) const {
        return std::get<T>(get(id));
    }

    template <typename T>
    bool isOfType(JoinPlanNodeId id) const {
        return std::holds_alternative<T>(get(id));
    }

    std::string joinPlansToString(const JoinPlans& plans,
                                  size_t numNodesToPrint = kHardMaxNodesInJoin,
                                  std::string indentStr = "") const;

    std::string joinPlanNodeToString(JoinPlanNodeId node,
                                     size_t numNodesToPrint = kHardMaxNodesInJoin,
                                     std::string indentStr = "") const;

    BSONObj joinPlanNodeToBSON(JoinPlanNodeId node,
                               const JoinGraph& graph,
                               size_t numNodesToPrint = kHardMaxNodesInJoin) const;

private:
    std::vector<JoinPlanNode> _allJoinPlans;
};
}  // namespace mongo::join_ordering
