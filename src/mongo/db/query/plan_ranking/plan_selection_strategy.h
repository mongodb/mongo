// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Identifies the strategy that selected the winning query plan. kSinglePlan/kCachedPlan
 * mean no ranking took place. kJoinOptimization identifies plans selected by join optimization, and
 * kJoinCachedPlan identifies a join-optimized plan recovered from the join plan cache instead of
 * re-running enumeration. kExpress identifies operations executed by the express path, which
 * bypasses query planning altogether.
 */
#define PLAN_SELECTION_STRATEGY_TABLE(X)     \
    X(kMultiPlanner, "multiPlanning")        \
    X(kCostBasedRanker, "costBased")         \
    X(kSinglePlan, "singlePlan")             \
    X(kCachedPlan, "cachedPlan")             \
    X(kExpress, "express")                   \
    X(kJoinOptimization, "joinOptimization") \
    X(kJoinCachedPlan, "joinCachedPlan")

QUERY_UTIL_NAMED_ENUM_DEFINE(PlanSelectionStrategy, PLAN_SELECTION_STRATEGY_TABLE)
#undef PLAN_SELECTION_STRATEGY_TABLE

inline std::string_view getPlanSelectionStrategyName(PlanSelectionStrategy strategy) {
    return toStringData(strategy);
}

// An absent strategy is how diagnostics report an operation that selected no plan at all.
inline std::string_view getPlanSelectionStrategyName(
    const boost::optional<PlanSelectionStrategy>& strategy) {
    return strategy ? getPlanSelectionStrategyName(*strategy) : std::string_view{"none"};
}
}  // namespace mongo
