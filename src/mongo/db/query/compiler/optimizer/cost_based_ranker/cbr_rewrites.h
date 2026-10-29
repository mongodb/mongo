// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/util/modules.h"

/**
 * This file containf rewrites from IndexBounds to MatchExpression for the purpose of cardinality
 * estimation.
 */

namespace mongo::cost_based_ranker {
/**
 * Create a match expression equivalent to the index bounds intervals and a possible filter
 * expression.
 * Params: 'bounds': index bounds from a IndexScanNode,
 *         'filterExpr': a filter expression of the IndexScanNode, in case of inexact match,
 *          or a filter expression in the parent FetchNode.
 * If the 'filterExpr' is not null, create a conjunction of the index-bounds expression
 * and the filter expression.
 * The function returns nullptr if it encounters an interval in the index bounds for which
 * the transformation to match expression is not supported. The nullptr notifies the
 * caller to use an alternative implementation.
 * The final condition that combines the expression generated from 'bounds' and 'filterExpr' are
 * simplified and normalized before returning. This is done to enable faster estimation with fewer
 * terms, and to also ensure that logically equivalent expressions have the same form, and thus
 * will be detected as equivalent by the CE cache.
 */
std::unique_ptr<MatchExpression> getMatchExpressionFromBounds(const IndexBounds& bounds,
                                                              const MatchExpression* filterExpr);
}  // namespace mongo::cost_based_ranker
