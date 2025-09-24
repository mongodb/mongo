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
