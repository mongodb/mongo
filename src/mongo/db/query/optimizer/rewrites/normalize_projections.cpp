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

#include "mongo/db/query/optimizer/rewrites/normalize_projections.h"

#include "mongo/db/query/optimizer/algebra/operator.h"

namespace mongo::optimizer {

ProjNormalize::ProjNormalize(const ProjNormalize::RenamedProjFn& renamedProjFn, PrefixId& prefixId)
    : _renamedProjFn(renamedProjFn), _prefixId(prefixId) {}

bool ProjNormalize::optimize(ABT& n) {
    algebra::transport<true>(n, *this);
    return false;
}

void ProjNormalize::transport(ABT& n, const Variable& var) {
    n = make<Variable>(renameProj(var.name()));
}

void ProjNormalize::prepare(const ABT& n, const Let& node) {
    // Rename projection.
    auto newProj = _prefixId.getNextId("renamed");
    if (_renamedProjFn) {
        _renamedProjFn(node.varName(), newProj);
    }
    _renames.emplace(node.varName(), std::move(newProj));
}

void ProjNormalize::transport(ABT& n, const Let& let, ABT& inBind, ABT& inExpr) {
    n = make<Let>(renameProj(let.varName()), std::move(inBind), std::move(inExpr));
}

void ProjNormalize::prepare(const ABT& n, const LambdaAbstraction& node) {
    // Rename projection.
    auto newProj = _prefixId.getNextId("renamed");
    if (_renamedProjFn) {
        _renamedProjFn(node.varName(), newProj);
    }
    _renames.emplace(node.varName(), std::move(newProj));
}

void ProjNormalize::transport(ABT& n, const LambdaAbstraction& abstr, ABT& body) {
    n = make<LambdaAbstraction>(renameProj(abstr.varName()), std::move(body));
}

void ProjNormalize::transport(ABT& n, const ExpressionBinder& binders, ABTVector& children) {
    // Rename all variables create via bindings. Call rename handler if present.

    ProjectionNameVector newNames;
    for (const ProjectionName& projName : binders.names()) {
        ProjectionName newProj = _prefixId.getNextId("renamed");
        if (_renamedProjFn) {
            _renamedProjFn(projName, newProj);
        }
        _renames.emplace(projName, newProj);
        newNames.push_back(std::move(newProj));
    }
    n = make<ExpressionBinder>(std::move(newNames), std::move(children));
}

void ProjNormalize::transport(ABT& n, const PhysicalScanNode& node, ABT& bind) {
    n = make<PhysicalScanNode>(renameFieldProjectionMap(node.getFieldProjectionMap()),
                               node.getScanDefName(),
                               node.useParallelScan(),
                               node.getScanOrder());
}

void ProjNormalize::transport(ABT& n, const IndexScanNode& node, ABT& bind) {
    n = make<IndexScanNode>(renameFieldProjectionMap(node.getFieldProjectionMap()),
                            node.getScanDefName(),
                            node.getIndexDefName(),
                            node.getIndexInterval(),
                            node.isIndexReverseOrder());
}

void ProjNormalize::transport(ABT& n, const SeekNode& node, ABT& bind, ABT& refs) {
    // The RID projection name is obtained via the embedded References and its underlying Variable
    // is already renamed.
    n = make<SeekNode>(node.getRIDProjectionName(),
                       renameFieldProjectionMap(node.getFieldProjectionMap()),
                       node.getScanDefName());
}

void ProjNormalize::transport(
    ABT& n, const HashJoinNode& node, ABT& leftChild, ABT& rightChild, ABT& refs) {
    n = make<HashJoinNode>(node.getJoinType(),
                           renameVector(node.getLeftKeys()),
                           renameVector(node.getRightKeys()),
                           std::move(leftChild),
                           std::move(rightChild));
}

void ProjNormalize::transport(
    ABT& n, const MergeJoinNode& node, ABT& leftChild, ABT& rightChild, ABT& refs) {
    n = make<MergeJoinNode>(renameVector(node.getLeftKeys()),
                            renameVector(node.getRightKeys()),
                            node.getCollation(),
                            std::move(leftChild),
                            std::move(rightChild));
}

void ProjNormalize::transport(
    ABT& n, const SortedMergeNode& node, ABTVector& children, ABT& binds, ABT& refs) {
    n = make<SortedMergeNode>(renameCollSpec(node.getCollationSpec()), std::move(children));
}

void ProjNormalize::transport(
    ABT& n, const NestedLoopJoinNode& node, ABT& leftChild, ABT& rightChild, ABT& filter) {
    ProjectionNameSet correlated;
    for (const auto& p : node.getCorrelatedProjectionNames()) {
        correlated.insert(renameProj(p));
    }
    n = make<NestedLoopJoinNode>(node.getJoinType(),
                                 std::move(correlated),
                                 std::move(filter),
                                 std::move(leftChild),
                                 std::move(rightChild));
}

void ProjNormalize::prepare(const ABT& n, const UnwindNode& node) {
    // Since the unwind node's output binding replaces the input binding, save the variable before
    // it gets renamed via the ExpressionBinder.
    _bindingsStack.push_back(node.binder().names());
}

void ProjNormalize::transport(ABT& n, const UnwindNode& node, ABT& child, ABT& binds, ABT& refs) {
    const auto& originalProjs = _bindingsStack.back();
    n = make<UnwindNode>(renameProj(originalProjs.at(0)),
                         renameProj(originalProjs.at(1)),
                         node.getRetainNonArrays(),
                         std::move(child));
    _bindingsStack.pop_back();
}

void ProjNormalize::prepare(const ABT& n, const UnionNode& node) {
    // Since the union node's output bindings replace the input bindings, save them before they are
    // renamed via the ExpressionBinder.
    _bindingsStack.push_back(node.binder().names());
}

void ProjNormalize::transport(
    ABT& n, const UnionNode& node, ABTVector& children, ABT& binds, ABT& refs) {
    n = make<UnionNode>(renameVector(_bindingsStack.back()), std::move(children));
    _bindingsStack.pop_back();
}

void ProjNormalize::prepare(const ABT& n, const GroupByNode& node) {
    // Since the groupby node's group-by bindings replace the input bindings, save them before they
    // are renamed via the ExpressionBinder.
    _bindingsStack.push_back(node.getGroupByProjectionNames());
}

void ProjNormalize::transport(ABT& n,
                              const GroupByNode& node,
                              ABT& child,
                              ABT& bindAgg,
                              ABT& refsAgg,
                              ABT& bindGb,
                              ABT& refsGb) {
    n = make<GroupByNode>(renameVector(_bindingsStack.back()),
                          node.getAggregationProjectionNames(),
                          node.getAggregationExpressions(),
                          std::move(child));
    _bindingsStack.pop_back();
}

void ProjNormalize::transport(ABT& n, const UniqueNode& node, ABT& child, ABT& refs) {
    n = make<UniqueNode>(renameVector(node.getProjections()), std::move(child));
}

void ProjNormalize::transport(ABT& n, const CollationNode& node, ABT& child, ABT& refs) {
    n = make<CollationNode>(renameCollSpec(node.getCollationSpec()), std::move(child));
}

void ProjNormalize::transport(ABT& n, const ExchangeNode& node, ABT& child, ABT& refs) {
    auto prop = node.getProperty();
    for (auto& p : prop.getDistributionAndProjections()._projectionNames) {
        p = renameProj(p);
    }
    n = make<ExchangeNode>(std::move(prop), std::move(child));
}

void ProjNormalize::transport(ABT& n, const RootNode& node, ABT& child, ABT& refs) {
    n = make<RootNode>(
        ProjectionNameOrderPreservingSet{renameVector(node.getProjections().getVector())},
        std::move(child));
}

ProjectionName ProjNormalize::renameProj(const ProjectionName& proj) const {
    return _renames.at(proj);
}

FieldProjectionMap ProjNormalize::renameFieldProjectionMap(const FieldProjectionMap& fpm) const {
    FieldProjectionMap result = fpm;

    if (auto& proj = result._ridProjection) {
        proj = renameProj(*proj);
    }
    if (auto& proj = result._rootProjection) {
        proj = renameProj(*proj);
    }
    for (auto& [fieldName, projName] : result._fieldProjections) {
        projName = renameProj(projName);
    }

    return result;
}

ProjectionNameVector ProjNormalize::renameVector(const ProjectionNameVector& v) const {
    ProjectionNameVector result;
    for (const auto& p : v) {
        result.push_back(renameProj(p));
    }
    return result;
}

ProjectionCollationSpec ProjNormalize::renameCollSpec(ProjectionCollationSpec spec) const {
    for (auto& [projName, op] : spec) {
        projName = renameProj(projName);
    }
    return spec;
}

}  // namespace mongo::optimizer
