// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

namespace mongo::ce {
using CardinalityEstimate = mongo::cost_based_ranker::CardinalityEstimate;
using CEResult = StatusWith<CardinalityEstimate>;

/**
 * This interface exposes an exact cardinality calculator for query plans, populating cardinalities
 * in the estimates map that is passed to cost estimation. Even though this abstract base class only
 * has one implementation, we need it to break a dependency cycle.
 */
class ExactCardinalityEstimator {
public:
    virtual ~ExactCardinalityEstimator() {}
    /**
     * Calculates the exact cardinalities for a given plan, storing them in the cardinalities map.
     */
    virtual CEResult calculateExactCardinality(
        const QuerySolution& plan, cost_based_ranker::EstimateMap& cardinalities) const = 0;
};
}  // namespace mongo::ce
