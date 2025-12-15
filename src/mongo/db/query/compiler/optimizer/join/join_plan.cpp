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
NodeSet getBitset(const JoinPlanNode& node) {
    return std::visit(
        OverloadedVisitor{[](const JoiningNode& join) { return join.bitset; },
                          [](const INLJRHSNode& ip) { return NodeSet().set(ip.node); },
                          [](const BaseNode& base) {
                              return NodeSet().set(base.node);
                          }},
        node);
}

/**
 * Helper to pretty-print NodeSet.
 */
std::string nodeSetToString(const NodeSet& set, size_t numNodesToPrint = kMaxNodesInJoin) {
    return set.to_string().substr(kMaxNodesInJoin - numNodesToPrint, numNodesToPrint);
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
    return str::stream() << indentStr << "[" << nodeSetToString(getBitset(node), numNodesToPrint)
                         << "]";
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
    return str::stream() << subset.to_string().substr(kMaxNodesInJoin - numNodesToPrint,
                                                      numNodesToPrint);
}

std::ostream& operator<<(std::ostream& os, const JoinSubset& subset) {
    return os << subset.toString();
}

JoinPlanNodeId JoinPlanNodeRegistry::registerJoinNode(const JoinSubset& subset,
                                                      JoinMethod method,
                                                      JoinPlanNodeId left,
                                                      JoinPlanNodeId right) {
    JoinPlanNodeId id = _allJoinPlans.size();
    _allJoinPlans.emplace_back(JoiningNode{method, left, right, subset.subset});
    return id;
}

JoinPlanNodeId JoinPlanNodeRegistry::registerBaseNode(NodeId node,
                                                      const QuerySolution* soln,
                                                      const NamespaceString& nss) {
    tassert(11371702, "Expected an initialized qsn", soln);
    JoinPlanNodeId id = _allJoinPlans.size();
    _allJoinPlans.emplace_back(BaseNode{soln, nss, node});
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
    return std::visit(
        OverloadedVisitor{[](const JoiningNode& join) { return join.bitset; },
                          [](const INLJRHSNode& ip) { return NodeSet().set(ip.node); },
                          [](const BaseNode& base) {
                              return NodeSet().set(base.node);
                          }},
        get(id));
}

BSONObj JoinPlanNodeRegistry::joinPlanNodeToBSON(JoinPlanNodeId nodeId,
                                                 size_t numNodesToPrint) const {
    return std::visit(
        OverloadedVisitor{
            [this, numNodesToPrint](const JoiningNode& join) {
                return BSON("subset" << nodeSetToString(join.bitset, numNodesToPrint) << "method"
                                     << joinMethodToString(join.method) << "left"
                                     << joinPlanNodeToBSON(join.left, numNodesToPrint) << "right"
                                     << joinPlanNodeToBSON(join.right, numNodesToPrint));
            },
            [numNodesToPrint](const INLJRHSNode& ip) {
                return BSON("subset" << nodeSetToString(ip.node, numNodesToPrint) << "accessPath"
                                     << (str::stream() << "INDEX_PROBE "
                                                       << ip.entry->descriptor()->keyPattern()));
            },
            [numNodesToPrint](const BaseNode& base) {
                return BSON("subset" << nodeSetToString(base.node, numNodesToPrint) << "accessPath"
                                     << base.soln->summaryString());
            }},
        get(nodeId));
}

}  // namespace mongo::join_ordering
