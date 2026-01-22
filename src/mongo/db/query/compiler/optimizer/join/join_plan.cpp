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

#include "mongo/db/query/compiler/optimizer/join/join_plan.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/str.h"

namespace mongo::join_ordering {
namespace {
JoinCostEstimate getNodeCost(const JoinPlanNode& node) {
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

NodeSet getNodeBitset(const JoinPlanNode& node) {
    return std::visit(
        OverloadedVisitor{[](const JoiningNode& join) { return join.bitset; },
                          [](const INLJRHSNode& ip) { return NodeSet().set(ip.node); },
                          [](const BaseNode& base) {
                              return NodeSet().set(base.node);
                          }},
        node);
}

/**
 * Helper to pretty-print join method.
 */
std::string joinMethodToString(JoinMethod method) {
    switch (method) {
        case JoinMethod::HJ:
            return "HJ";
        case JoinMethod::NLJ:
            return "NLJ";
        case JoinMethod::INLJ:
            return "INLJ";
    }

    MONGO_UNREACHABLE_TASSERT(11336901);
}

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
                                                 size_t numNodesToPrint) const {
    BSONObjBuilder bob;
    bob << "subset" << nodeSetToString(getBitset(nodeId), numNodesToPrint);
    if (!isOfType<INLJRHSNode>(nodeId)) {
        // INLJRHSNodes don't have their own associated cost.
        bob << "cost" << getCost(nodeId).toBSON();
    }
    std::visit(
        OverloadedVisitor{[this, numNodesToPrint, &bob](const JoiningNode& join) {
                              bob << "method" << joinMethodToString(join.method);
                              bob << "left" << joinPlanNodeToBSON(join.left, numNodesToPrint);
                              bob << "right" << joinPlanNodeToBSON(join.right, numNodesToPrint);
                          },
                          [&bob](const INLJRHSNode& ip) {
                              bob << "accessPath"
                                  << (str::stream()
                                      << "INDEX_PROBE " << ip.entry->descriptor()->keyPattern());
                          },
                          [&bob](const BaseNode& base) {
                              bob << "accessPath" << base.soln->summaryString();
                          }},
        get(nodeId));
    return bob.obj();
}

}  // namespace mongo::join_ordering
