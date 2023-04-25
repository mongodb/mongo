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

#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"

namespace mongo::optimizer::ce {
/**
 * A tree of selectivity estimates for a certain boolean expression. The leaves of the tree contain
 * selectivity estimates of some nodes. The internal nodes encode the Boolean structure of the
 * expression being estimated but they do not contain the internal node estimates themselves.
 * This tree is passed to a Boolean selectivity estimator that knows how to combine child estimates.
 */
using SelectivityTree = BoolExpr<SelectivityType>;
using SelectivityTreeBuilder =
    SelectivityTree::Builder<false /* simplifyEmptyOrSingular */, false /*removeDups*/>;

SelectivityType estimateSelectivityTree(const SelectivityTree::Node& selTree);

/**
 * Function to estimate one PartialSchemaEntry within a PartialSchemaRequirements expression. The
 * estimate should be incorporated into the builder.
 */
using EstimatePartialSchemaEntrySelFn =
    std::function<void(SelectivityTreeBuilder&, const PartialSchemaEntry&)>;

/**
 * Given a Boolean tree of PartialSchemaEntries, build a SelectivityTree with the same structure,
 * such that the leaf nodes of this tree contain selectivity estimates of the corresponding entries,
 * produce a selectivity estimate from the SelectivityTree, and use that to estimate the cardinality
 * of the requirements, given 'inputCE'. Leaf node selectivities are estimated via the provided
 * EstimatePartialSchemaEntrySelFn. Perf-only partial schema entries are not included in the
 * estimate. Return 'inputCE' if there are no entries to estimate.
 */
class PartialSchemaRequirementsCardinalityEstimator {
public:
    PartialSchemaRequirementsCardinalityEstimator(
        const EstimatePartialSchemaEntrySelFn& estimatePartialSchemEntryFn, CEType inputCE);

    void transport(const PSRExpr::Atom& atom);

    void prepare(const PSRExpr::Conjunction& node);
    void transport(const PSRExpr::Conjunction& node, const PSRExpr::NodeVector&);

    void prepare(const PSRExpr::Disjunction& node);
    void transport(const PSRExpr::Disjunction& node, const PSRExpr::NodeVector&);

    CEType estimateCE(const PSRExpr::Node& n);

private:
    const EstimatePartialSchemaEntrySelFn& _estimatePartialSchemEntryFn;
    const CEType _inputCE;
    SelectivityTreeBuilder _selTreeBuilder;
};

/**
 * Function to estimate one IntervalRequirement within a IntervalReqExpr. The estimate should be
 * incorporated into the builder.
 */
using EstimateIntervalSelFn =
    std::function<void(SelectivityTreeBuilder&, const IntervalRequirement&)>;

/**
 * Given a Boolean tree of intervals build a SelectivityTree with the same structure, such that the
 * leaf nodes of this tree contain selectivity estimates of the corresponding intervals. Leaf node
 * selectivities are estimated via the provided EstimateIntervalSelFn.
 */
class IntervalSelectivityTreeBuilder {
public:
    IntervalSelectivityTreeBuilder(SelectivityTreeBuilder& selTreeBuilder,
                                   const EstimateIntervalSelFn& estimateIntervalSelFn);

    void transport(const IntervalReqExpr::Atom& node);

    void prepare(const IntervalReqExpr::Conjunction& node);

    void transport(const IntervalReqExpr::Conjunction& node,
                   const IntervalReqExpr::NodeVector& /* children */);

    void prepare(const IntervalReqExpr::Disjunction& node);
    void transport(const IntervalReqExpr::Disjunction& node,
                   const IntervalReqExpr::NodeVector& /* children */);

    void build(const IntervalReqExpr::Node& intervalTree);

private:
    const EstimateIntervalSelFn& _estimateIntervalSelFn;
    SelectivityTreeBuilder& _selTreeBuilder;
};
}  // namespace mongo::optimizer::ce
