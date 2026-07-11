// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <span>

namespace mongo::cost_based_ranker {

/**
 * Specifies the maximum number of elements (selectivities) to use when estimating via
 * exponential backoff.
 */
constexpr size_t kMaxBackoffElements = 4;

/**
 * Estimates the selectivity of a conjunction given the selectivities of its subexpressions using
 * exponential backoff.
 */
SelectivityEstimate conjExponentialBackoff(std::span<SelectivityEstimate> conjSelectivities);

/**
 * Estimates the selectivity of a disjunction given the selectivities of its subexpressions using
 * exponential backoff.
 */
SelectivityEstimate disjExponentialBackoff(std::span<SelectivityEstimate> disjSelectivities);

void addFieldsToRelevantIndexOutput(const BSONObj& keyPattern, StringSet& relevantIndexOutput);

/**
 * Returns true if the CE/costing of the given StageType is not yet supported by CBR.
 */
bool isNodeUnsupportedByCBR(StageType type);

/**
 * Returns true if CBR should never encounter this StageType.
 */
bool isNodeUnexpectedByCBR(StageType type);
}  // namespace mongo::cost_based_ranker
