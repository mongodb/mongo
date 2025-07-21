/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"

namespace mongo::cost_based_ranker {

constexpr double kEqualityScalingFactor = 0.5;
constexpr double kTextSearchScalingFactor = 0.4;
constexpr double kDefaultScalingFactor = 0.3;
constexpr double kAverageElementsPerArray = 7.0;
const SelectivityEstimate kIsArraySel =
    SelectivityEstimate{SelectivityType{0.9}, EstimationSource::Heuristics};
const SelectivityEstimate kIsObjectSel =
    SelectivityEstimate{SelectivityType{0.9}, EstimationSource::Heuristics};
const SelectivityEstimate kExistsSel{SelectivityType{0.9}, EstimationSource::Heuristics};
const SelectivityEstimate oneSelHeuristic{SelectivityType{1}, EstimationSource::Heuristics};
const SelectivityEstimate zeroSelHeuristic{SelectivityType{0}, EstimationSource::Heuristics};

/**
 * Return true if an expression can be estimated via heuristics.
 */
bool heuristicIsEstimable(const MatchExpression* expr);

/**
 * Estimate the selectivity of the given 'MatchExpression'. The expression must be a leaf, that is
 * an atomic predicate. The caller is responsible for estimating selectivies of conjunctions,
 * disjuctions and negations.
 */
SelectivityEstimate heuristicLeafMatchExpressionSel(const MatchExpression* expr,
                                                    CardinalityEstimate inputCard);

/**
 * Estimate a single interval heuristically, depending on the available bounds.
 */
SelectivityEstimate estimateInterval(const Interval& interval, CardinalityEstimate inputCard);

}  // namespace mongo::cost_based_ranker
