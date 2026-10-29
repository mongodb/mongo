// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_all_indices_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <queue>
#include <string>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class PlanYieldPolicy;

/**
 * Runs a trial period in order to evaluate the cost of a cached plan. If the cost is unexpectedly
 * high, the plan cache entry is deactivated and we use multi-planning to select an entirely new
 * winning plan. This process is called "replanning".
 *
 * This stage requires all indices to stay intact during the trial period so that replanning can
 * occur with the set of indices in 'params'. As a future improvement, we could instead refresh the
 * list of indices in 'params' prior to replanning, and thus avoid inheriting from
 * RequiresAllIndicesStage.
 */
class CachedPlanStage final : public RequiresAllIndicesStage {
public:
    CachedPlanStage(ExpressionContext* expCtx,
                    CollectionAcquisition collection,
                    WorkingSet* ws,
                    const CanonicalQuery* cq,
                    size_t decisionWorks,
                    std::unique_ptr<PlanStage> root,
                    size_t cachedPlanHash = 0);

    bool isEOF() const final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_CACHED_PLAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "CACHED_PLAN"sv;

    /**
     * Runs the cached plan for a trial period, yielding during the trial period according to
     * 'yieldPolicy'.
     *
     * Feedback from the trial period is passed to the plan cache. If the performance is lower
     * than expected, the old plan is evicted and a new plan is selected from scratch (again
     * yielding according to 'yieldPolicy'). Otherwise, the cached plan is run.
     */
    Status pickBestPlan(const QueryPlannerParams& plannerParams, PlanYieldPolicy* yieldPolicy);

    bool bestPlanChosen() const {
        return _bestPlanChosen;
    }

private:
    /**
     * Returns a non-OK Status to restart the query planning flow to pick a new plan.
     *
     * We only modify the plan cache if 'shouldCache' is true.
     */
    Status replan(bool shouldCache, std::string reason);

    /**
     * May yield during the cached plan stage's trial period or replanning phases.
     *
     * Returns a non-OK status if query planning fails. In particular, this function returns
     * ErrorCodes::QueryPlanKilled if the query plan was killed during a yield, or
     * ErrorCodes::MaxTimeMSExpired if the operation exceeded its time limit.
     */
    Status tryYield(PlanYieldPolicy* yieldPolicy);

    // Not owned.
    WorkingSet* _ws;

    // Not owned.
    const CanonicalQuery* _canonicalQuery;

    // The number of work cycles taken to decide on a winning plan when the plan was first
    // cached.
    size_t _decisionWorks;

    // The hash of the QuerySolution that was retrieved from the plan cache. It is used to compare
    // against the result of replanning for metrics gathering purposes.
    const size_t _cachedPlanHash;

    // If we fall back to re-planning the query, and there is just one resulting query solution,
    // that solution is owned here.
    std::unique_ptr<QuerySolution> _replannedQs;

    // Any results produced during trial period execution are kept here.
    std::queue<WorkingSetID> _results;

    // Stats
    CachedPlanStats _specificStats;

    // Picked best plan
    bool _bestPlanChosen = false;
};

}  // namespace mongo
