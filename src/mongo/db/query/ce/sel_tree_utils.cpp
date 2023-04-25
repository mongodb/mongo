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

#include "mongo/db/query/ce/sel_tree_utils.h"

#include "mongo/db/query/optimizer/utils/ce_math.h"

namespace mongo::optimizer::ce {

namespace {

class SelectivityTreeEstimatorTransport {
public:
    SelectivityType transport(const SelectivityTree::Atom& node) {
        SelectivityType sel = node.getExpr();
        tassert(7454000, "Leaf nodes must have computed CE.", validSelectivity(sel));
        return sel;
    }

    SelectivityType transport(const SelectivityTree::Conjunction& node,
                              std::vector<SelectivityType> children) {
        SelectivityType conjSel = conjExponentialBackoff(std::move(children));
        tassert(7454001, "Failed to estimate conjunction.", validSelectivity(conjSel));
        return conjSel;
    }

    SelectivityType transport(const SelectivityTree::Disjunction& node,
                              std::vector<SelectivityType> children) {
        SelectivityType disjSel = disjExponentialBackoff(std::move(children));
        tassert(7454002, "Failed to estimate disjunction.", validSelectivity(disjSel));
        return disjSel;
    }

    SelectivityType estimate(const SelectivityTree::Node& selTree) {
        SelectivityType sel = algebra::transport<false>(selTree, *this);
        tassert(7454003, "Invalid selectivity.", validSelectivity(sel));
        return sel;
    }
};
}  // namespace

SelectivityType estimateSelectivityTree(const SelectivityTree::Node& selTree) {
    return SelectivityTreeEstimatorTransport{}.estimate(selTree);
}

PartialSchemaRequirementsCardinalityEstimator::PartialSchemaRequirementsCardinalityEstimator(
    const EstimatePartialSchemaEntrySelFn& estimatePartialSchemEntryFn, CEType inputCE)
    : _estimatePartialSchemEntryFn(estimatePartialSchemEntryFn), _inputCE(inputCE) {}

void PartialSchemaRequirementsCardinalityEstimator::transport(const PSRExpr::Atom& atom) {
    const auto& entry = atom.getExpr();

    // Ignore perf-only requirements.
    if (!entry.second.getIsPerfOnly()) {
        _estimatePartialSchemEntryFn(_selTreeBuilder, entry);
    }
}

void PartialSchemaRequirementsCardinalityEstimator::prepare(const PSRExpr::Conjunction& node) {
    _selTreeBuilder.pushConj();
}
void PartialSchemaRequirementsCardinalityEstimator::transport(
    const PSRExpr::Conjunction& node, const PSRExpr::NodeVector& /* children */) {
    _selTreeBuilder.pop();
}

void PartialSchemaRequirementsCardinalityEstimator::prepare(const PSRExpr::Disjunction& node) {
    _selTreeBuilder.pushDisj();
}
void PartialSchemaRequirementsCardinalityEstimator::transport(
    const PSRExpr::Disjunction& node, const PSRExpr::NodeVector& /* children */) {
    _selTreeBuilder.pop();
}

CEType PartialSchemaRequirementsCardinalityEstimator::estimateCE(const PSRExpr::Node& n) {
    algebra::transport<false>(n, *this);
    if (auto selTree = _selTreeBuilder.finish()) {
        return _inputCE * estimateSelectivityTree(*selTree);
    }

    return _inputCE;
}


IntervalSelectivityTreeBuilder::IntervalSelectivityTreeBuilder(
    SelectivityTreeBuilder& selTreeBuilder, const EstimateIntervalSelFn& estimateIntervalSelFn)
    : _estimateIntervalSelFn(estimateIntervalSelFn), _selTreeBuilder(selTreeBuilder) {}

void IntervalSelectivityTreeBuilder::transport(const IntervalReqExpr::Atom& node) {
    _estimateIntervalSelFn(_selTreeBuilder, node.getExpr());
}

void IntervalSelectivityTreeBuilder::prepare(const IntervalReqExpr::Conjunction& node) {
    _selTreeBuilder.pushConj();
}
void IntervalSelectivityTreeBuilder::transport(const IntervalReqExpr::Conjunction& node,
                                               const IntervalReqExpr::NodeVector& /* children */) {
    _selTreeBuilder.pop();
}

void IntervalSelectivityTreeBuilder::prepare(const IntervalReqExpr::Disjunction& node) {
    _selTreeBuilder.pushDisj();
}
void IntervalSelectivityTreeBuilder::transport(const IntervalReqExpr::Disjunction& node,
                                               const IntervalReqExpr::NodeVector& /* children */) {
    _selTreeBuilder.pop();
}

void IntervalSelectivityTreeBuilder::build(const IntervalReqExpr::Node& intervalTree) {
    algebra::transport<false>(intervalTree, *this);
}
}  // namespace mongo::optimizer::ce
