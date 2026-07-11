// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::cost_based_ranker {

/**
 * EstimateMap is a type representing a mapping from QuerySolutionNodes to cost estimates.
 */
using EstimateMap = absl::flat_hash_map<const QuerySolutionNode*, std::unique_ptr<QSNEstimate>>;

}  // namespace mongo::cost_based_ranker
