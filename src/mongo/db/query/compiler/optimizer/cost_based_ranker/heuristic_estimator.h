// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/util/modules.h"

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
