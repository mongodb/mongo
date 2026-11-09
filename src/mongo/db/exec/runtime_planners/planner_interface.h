// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/modules.h"

namespace mongo {

/*
 *  Common interface for planner implementations.
 */
class PlannerInterface {
public:
    virtual ~PlannerInterface() = default;

    /**
     * Extracts a `PlanRankingResult` which summarizes all information from the planning phase.
     * Only called when `featureFlagGetExecutorDeferredEngineChoice` is enabled.
     * TODO SERVER-119036 when the legacy get_executor is deleted, this function can be pure
     * virtual.
     */
    virtual PlanRankingResult extractPlanRankingResult() {
        MONGO_UNREACHABLE_TASSERT(11974308);
    }
};
}  // namespace mongo
