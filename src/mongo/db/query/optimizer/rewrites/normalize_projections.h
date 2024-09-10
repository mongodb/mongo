/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

class ProjNormalize {
public:
    // Handler which is called when we inline a projection name (target) with another projection
    // name (source).
    using RenamedProjFn =
        std::function<void(const ProjectionName& target, const ProjectionName& source)>;

    ProjNormalize(const RenamedProjFn& renamedProjFn, PrefixId& prefixId);

    bool optimize(ABT& n);

    template <typename T, typename... Ts>
    void transport(ABT& n, const T&, Ts&&...) {
        tassert(8080800,
                "Should not be seeing logical nodes in this context",
                !n.is<ExclusivelyLogicalNode>());
    }

    /**
     * Expressions.
     */
    void transport(ABT& n, const Variable& var);

    /**
     * Let and LambdaAbstraciton introduce a binding.
     */
    void prepare(const ABT& n, const Let& node);
    void transport(ABT& n, const Let& let, ABT& inBind, ABT& inExpr);
    void prepare(const ABT& n, const LambdaAbstraction& node);
    void transport(ABT& n, const LambdaAbstraction& abstr, ABT& body);

    void transport(ABT& n, const ExpressionBinder& binders, std::vector<ABT>& children);

    /**
     * Physical Nodes.
     */
    void transport(ABT& n, const PhysicalScanNode& node, ABT& bind);
    void transport(ABT& n, const IndexScanNode& node, ABT& bind);
    void transport(ABT& n, const SeekNode& node, ABT& bind, ABT& refs);
    void transport(ABT& n, const HashJoinNode& node, ABT& leftChild, ABT& rightChild, ABT& refs);
    void transport(ABT& n, const MergeJoinNode& node, ABT& leftChild, ABT& rightChild, ABT& refs);
    void transport(ABT& n, const SortedMergeNode& node, ABTVector& children, ABT& binds, ABT& refs);
    void transport(
        ABT& n, const NestedLoopJoinNode& node, ABT& leftChild, ABT& rightChild, ABT& filter);

    // Nodes which re-bind.
    void prepare(const ABT& n, const UnwindNode& node);
    void transport(ABT& n, const UnwindNode& node, ABT& child, ABT& binds, ABT& refs);
    void prepare(const ABT& n, const UnionNode& node);
    void transport(ABT& n, const UnionNode& node, ABTVector& children, ABT& binds, ABT& refs);
    void prepare(const ABT& n, const GroupByNode& node);
    void transport(ABT& n,
                   const GroupByNode& node,
                   ABT& child,
                   ABT& bindAgg,
                   ABT& refsAgg,
                   ABT& bindGb,
                   ABT& refsGb);

    void transport(ABT& n, const UniqueNode& node, ABT& child, ABT& refs);
    void transport(ABT& n, const CollationNode& node, ABT& child, ABT& refs);
    void transport(ABT& n, const ExchangeNode& node, ABT& child, ABT& refs);
    void transport(ABT& n, const RootNode& node, ABT& child, ABT& refs);

private:
    ProjectionName renameProj(const ProjectionName& proj) const;

    FieldProjectionMap renameFieldProjectionMap(const FieldProjectionMap& fpm) const;

    ProjectionNameVector renameVector(const ProjectionNameVector& v) const;

    ProjectionCollationSpec renameCollSpec(ProjectionCollationSpec spec) const;

    // Handler called when a projection is renamed.
    const RenamedProjFn& _renamedProjFn;

    PrefixId& _prefixId;

    // Map from original to renamed projection name.
    ProjectionRenames _renames;

    std::vector<ProjectionNameVector> _bindingsStack;
};

}  // namespace mongo::optimizer
