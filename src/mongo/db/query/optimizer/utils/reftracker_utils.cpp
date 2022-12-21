/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/utils/reftracker_utils.h"

#include "mongo/db/query/optimizer/reference_tracker.h"


namespace mongo::optimizer {


/**
 * Helper class used to extract variable references from a node.
 */
class NodeVariableTracker {
public:
    template <typename T, typename... Ts>
    ProjectionNameSet walk(const T&, Ts&&...) {
        static_assert(!std::is_base_of_v<Node, T>, "Nodes must implement variable tracking");

        // Default case: no variables.
        return {};
    }

    ProjectionNameSet walk(const ScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    ProjectionNameSet walk(const ValueScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    ProjectionNameSet walk(const PhysicalScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    ProjectionNameSet walk(const CoScanNode& /*node*/) {
        return {};
    }

    ProjectionNameSet walk(const IndexScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    ProjectionNameSet walk(const SeekNode& /*node*/, const ABT& /*binds*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const MemoLogicalDelegatorNode& /*node*/) {
        return {};
    }

    ProjectionNameSet walk(const MemoPhysicalDelegatorNode& /*node*/) {
        return {};
    }

    ProjectionNameSet walk(const FilterNode& /*node*/, const ABT& /*child*/, const ABT& expr) {
        return extractFromABT(expr);
    }

    ProjectionNameSet walk(const EvaluationNode& /*node*/, const ABT& /*child*/, const ABT& expr) {
        return extractFromABT(expr);
    }

    ProjectionNameSet walk(const SargableNode& /*node*/,
                           const ABT& /*child*/,
                           const ABT& /*binds*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const RIDIntersectNode& /*node*/,
                           const ABT& /*leftChild*/,
                           const ABT& /*rightChild*/) {
        return {};
    }

    ProjectionNameSet walk(const RIDUnionNode& /*node*/,
                           const ABT& /*leftChild*/,
                           const ABT& /*rightChild*/) {
        return {};
    }

    ProjectionNameSet walk(const BinaryJoinNode& /*node*/,
                           const ABT& /*leftChild*/,
                           const ABT& /*rightChild*/,
                           const ABT& expr) {
        return extractFromABT(expr);
    }

    ProjectionNameSet walk(const HashJoinNode& /*node*/,
                           const ABT& /*leftChild*/,
                           const ABT& /*rightChild*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const MergeJoinNode& /*node*/,
                           const ABT& /*leftChild*/,
                           const ABT& /*rightChild*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const SortedMergeNode& /*node*/,
                           const ABTVector& /*children*/,
                           const ABT& /*binder*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const NestedLoopJoinNode& /*node*/,
                           const ABT& /*leftChild*/,
                           const ABT& /*rightChild*/,
                           const ABT& expr) {
        return extractFromABT(expr);
    }

    ProjectionNameSet walk(const UnionNode& /*node*/,
                           const ABTVector& /*children*/,
                           const ABT& /*binder*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const GroupByNode& /*node*/,
                           const ABT& /*child*/,
                           const ABT& /*aggBinder*/,
                           const ABT& aggRefs,
                           const ABT& /*groupbyBinder*/,
                           const ABT& groupbyRefs) {
        ProjectionNameSet result;
        extractFromABT(result, aggRefs);
        extractFromABT(result, groupbyRefs);
        return result;
    }

    ProjectionNameSet walk(const UnwindNode& /*node*/,
                           const ABT& /*child*/,
                           const ABT& /*binds*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const UniqueNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const SpoolProducerNode& /*node*/,
                           const ABT& /*child*/,
                           const ABT& /*filter*/,
                           const ABT& /*binds*/,
                           const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const SpoolConsumerNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    ProjectionNameSet walk(const CollationNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const LimitSkipNode& /*node*/, const ABT& /*child*/) {
        return {};
    }

    ProjectionNameSet walk(const ExchangeNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    ProjectionNameSet walk(const RootNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    static ProjectionNameSet collect(const ABT& n) {
        NodeVariableTracker tracker;
        return algebra::walk<false>(n, tracker);
    }

private:
    void extractFromABT(ProjectionNameSet& vars, const ABT& v) {
        // Mark variables as defined or not in this subtree.
        ProjectionNameMap<bool> varHasDefinitionMap;
        VariableEnvironment::walkVariables(
            v,
            [&](const Variable& var) { varHasDefinitionMap.emplace(var.name(), false); },
            [&](const ProjectionName& definedVar) { varHasDefinitionMap[definedVar] = true; });
        for (const auto& varHasDefinition : varHasDefinitionMap) {
            if (!varHasDefinition.second) {
                // We are interested in when the variable has no definition in this subtree of the
                // ABT: either free variables, or variables defined on other nodes.
                vars.insert(varHasDefinition.first);
            }
        }
    }

    ProjectionNameSet extractFromABT(const ABT& v) {
        ProjectionNameSet result;
        extractFromABT(result, v);
        return result;
    }
};

ProjectionNameSet collectVariableReferences(const ABT& n) {
    return NodeVariableTracker::collect(n);
}

}  // namespace mongo::optimizer
