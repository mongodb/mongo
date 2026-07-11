// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_solution_analyzer.h"
#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"
#include "mongo/db/query/util/named_enum.h"

#include <cstddef>

namespace mongo {
namespace plan_shape_counters {

/**
 * Returns a state machine that matches a plan shape pattern against a given QSN.
 */
query_solution_analyzer::StateMachineMatcher makePlanShapeMatcher();

}  // namespace plan_shape_counters
}  // namespace mongo
