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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/util/str.h"

namespace mongo::join_ordering {
namespace {
/**
 * Helper to pretty-print NodeSet.
 */
std::string nodeSetToString(const NodeSet& set, size_t numNodesToPrint = kMaxNodesInJoin) {
    return set.to_string().substr(kMaxNodesInJoin - numNodesToPrint, numNodesToPrint);
}
}  // namespace

std::string BaseNode::toString(size_t numNodesToPrint, std::string indentStr) const {
    return str::stream() << indentStr << "[" << nodeSetToString(bitset, numNodesToPrint) << "]["
                         << nss.toString_forTest() << "] " << soln->summaryString();
}

std::string JoiningNode::toString(size_t numNodesToPrint, std::string indentStr) const {
    auto ss = std::stringstream() << indentStr << "[" << nodeSetToString(bitset, numNodesToPrint)
                                  << "] ";
    const auto methodStr = [this]() {
        switch (method) {
            case JoinMethod::HJ:
                return "HJ";
            case JoinMethod::NLJ:
                return "NLJ";
            case JoinMethod::INLJ:
                return "INLJ";
        }

        MONGO_UNREACHABLE_TASSERT(11336901);
    }();

    ss << methodStr;
    return ss.str();
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

JoinPlanNodeId JoinPlanNodeRegistry::registerBaseNode(const JoinSubset& subset,
                                                      const QuerySolution* soln,
                                                      const NamespaceString& nss) {
    JoinPlanNodeId id = _allJoinPlans.size();
    _allJoinPlans.emplace_back(BaseNode{soln, nss, subset.subset});
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

}  // namespace mongo::join_ordering
