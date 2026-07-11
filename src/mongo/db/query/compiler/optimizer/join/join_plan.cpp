// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_plan.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/util/disjoint_set.h"
#include "mongo/util/str.h"

namespace mongo::join_ordering {
namespace {

std::string joinNodeStringPrefix(const JoinPlanNode& node,
                                 size_t numNodesToPrint,
                                 std::string indentStr) {
    std::stringstream ss;
    ss << indentStr;
    if (!std::holds_alternative<INLJRHSNode>(node)) {
        // INLJRHSNodes don't have their own associated cost.
        ss << "(" << getNodeCost(node).toString() << ")";
    }
    ss << "[" << nodeSetToString(getNodeBitset(node), numNodesToPrint) << "]";
    return ss.str();
}
}  // namespace

std::string BaseNode::toString(size_t numNodesToPrint, std::string indentStr) const {
    return str::stream() << joinNodeStringPrefix(*this, numNodesToPrint, indentStr) << "["
                         << nss.toString_forTest() << "] " << soln->summaryString();
}

std::string INLJRHSNode::toString(size_t numNodesToPrint, std::string indentStr) const {
    return str::stream() << joinNodeStringPrefix(*this, numNodesToPrint, indentStr) << "["
                         << nss.toString_forTest() << "] INDEX_PROBE "
                         << entry->descriptor()->keyPattern();
}

std::string JoiningNode::toString(size_t numNodesToPrint, std::string indentStr) const {
    return str::stream() << joinNodeStringPrefix(*this, numNodesToPrint, indentStr) << " "
                         << joinMethodToString(method);
}

std::string JoinSubset::toString(size_t numNodesToPrint) const {
    return str::stream() << subset.to_string().substr(kHardMaxNodesInJoin - numNodesToPrint,
                                                      numNodesToPrint);
}

std::ostream& operator<<(std::ostream& os, const JoinSubset& subset) {
    return os << subset.toString();
}

JoinPlanNodeId JoinPlanNodeRegistry::registerJoinNode(const JoinSubset& subset,
                                                      JoinMethod method,
                                                      JoinPlanNodeId left,
                                                      JoinPlanNodeId right,
                                                      JoinCostEstimate cost) {
    JoinPlanNodeId id = _allJoinPlans.size();
    _allJoinPlans.emplace_back(JoiningNode{method, left, right, subset.subset, std::move(cost)});
    return id;
}

JoinPlanNodeId JoinPlanNodeRegistry::registerBaseNode(NodeId node,
                                                      const QuerySolution* soln,
                                                      const NamespaceString& nss,
                                                      JoinCostEstimate cost) {
    tassert(11371702, "Expected an initialized qsn", soln);
    JoinPlanNodeId id = _allJoinPlans.size();
    _allJoinPlans.emplace_back(BaseNode{soln, nss, node, std::move(cost)});
    return id;
}

JoinPlanNodeId JoinPlanNodeRegistry::registerINLJRHSNode(
    NodeId node, std::shared_ptr<const IndexCatalogEntry> entry, const NamespaceString& nss) {
    JoinPlanNodeId id = _allJoinPlans.size();
    _allJoinPlans.emplace_back(INLJRHSNode{std::move(entry), nss, node});
    return id;
}

std::string JoinPlanNodeRegistry::joinPlanNodeToString(JoinPlanNodeId nodeId,
                                                       size_t numNodesToPrint,
                                                       std::string indentStr) const {
    auto ss = std::stringstream();
    const auto& node = get(nodeId);
    std::visit(OverloadedVisitor{
                   [&indentStr, &ss, numNodesToPrint](const BaseNode& n) {
                       ss << n.toString(numNodesToPrint, indentStr);
                   },
                   [&indentStr, &ss, numNodesToPrint](const INLJRHSNode& n) {
                       ss << n.toString(numNodesToPrint, indentStr);
                   },
                   [this, &indentStr, &ss, numNodesToPrint](const JoiningNode& n) {
                       std::string nxtIndent = str::stream() << indentStr + "    ";
                       ss << n.toString(numNodesToPrint, indentStr) << "\n"
                          << indentStr << " -> left:\n"
                          << joinPlanNodeToString(n.left, numNodesToPrint, nxtIndent) << "\n"
                          << indentStr << " -> right:\n"
                          << joinPlanNodeToString(n.right, numNodesToPrint, nxtIndent);
                   },
               },
               node);
    return ss.str();
}

std::string JoinPlanNodeRegistry::joinPlansToString(const JoinPlans& plans,
                                                    size_t numNodesToPrint,
                                                    std::string indentStr) const {
    auto ss = std::stringstream();
    for (size_t i = 0; i < plans.size(); i++) {
        ss << i << "." << joinPlanNodeToString(plans[i], numNodesToPrint, indentStr) << "\n";
        if (i < plans.size() - 1) {
            ss << "\n";
        }
    }
    return ss.str();
}

NodeSet JoinPlanNodeRegistry::getBitset(JoinPlanNodeId id) const {
    return getNodeBitset(get(id));
}

JoinCostEstimate JoinPlanNodeRegistry::getCost(JoinPlanNodeId nodeId) const {
    return getNodeCost(get(nodeId));
}

BSONObj JoinPlanNodeRegistry::joinPlanNodeToBSON(JoinPlanNodeId nodeId,
                                                 const JoinGraph& graph,
                                                 size_t numNodesToPrint) const {
    const NodeSet bitset = getBitset(nodeId);
    BSONObjBuilder bob;
    bob << "subset" << nodeSetToString(bitset, numNodesToPrint);
    {
        BSONArrayBuilder namesBob(bob.subarrayStart("subsetCollectionNames"));
        for (const auto& name : subsetCollectionNames(bitset, graph)) {
            namesBob << name;
        }
    }
    if (!isOfType<INLJRHSNode>(nodeId)) {
        // INLJRHSNodes don't have their own associated cost.
        bob << "cost" << getCost(nodeId).toBSON();
    }
    std::visit(OverloadedVisitor{[this, &graph, numNodesToPrint, &bob](const JoiningNode& join) {
                                     bob << "method" << joinMethodToString(join.method);
                                     bob << "left"
                                         << joinPlanNodeToBSON(join.left, graph, numNodesToPrint);
                                     bob << "right"
                                         << joinPlanNodeToBSON(join.right, graph, numNodesToPrint);
                                 },
                                 [&bob](const INLJRHSNode& ip) {
                                     bob << "accessPath"
                                         << (str::stream() << "INDEX_PROBE "
                                                           << ip.entry->descriptor()->keyPattern());
                                 },
                                 [&bob](const BaseNode& base) {
                                     bob << "accessPath" << base.soln->summaryString();
                                 }},
               get(nodeId));
    return bob.obj();
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
