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
 * mean no ranking took place.
 * TODO: SERVER-133432 Extend plan selection strategy for express path executions. We can add them
 * to the single plan category or add a new kExpress value.
 */
#define PLAN_SELECTION_STRATEGY_TABLE(X) \
    X(kMultiPlanner, "multiPlanning")    \
    X(kCostBasedRanker, "costBased")     \
    X(kSinglePlan, "singlePlan")         \
    X(kCachedPlan, "cachedPlan")

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
