// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

namespace mongo {

/**
 * Identifies which plan ranker selected the winning query plan for an operation. Distinct from the
 * execution engine (see PlanExecutor::QueryFramework); a plan picked by either ranker may execute
 * in the classic or SBE engine. Stays 'kNone' when no ranking took place, e.g. a single candidate
 * solution or a plan recovered from the cache.
 */
enum class PlanRankerMethod {
    kNone,
    kMultiPlanner,
    kCostBasedRanker,
};

}  // namespace mongo
