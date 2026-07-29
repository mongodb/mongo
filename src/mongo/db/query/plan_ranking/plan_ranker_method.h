// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"

#include <string_view>

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

inline std::string_view getPlanRankerMethodName(PlanRankerMethod method) {
    switch (method) {
        case PlanRankerMethod::kNone:
            return "none";
        case PlanRankerMethod::kMultiPlanner:
            return "multiPlanning";
        case PlanRankerMethod::kCostBasedRanker:
            return "costBased";
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace mongo
