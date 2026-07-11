// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/get_executor_helpers.h"

namespace mongo {

namespace classic_runtime_planner {

SubPlanner::SubPlanner(PlannerData plannerData) : ClassicPlannerInterface(std::move(plannerData)) {
    SubplanStage::PlanSelectionCallbacks callbacks{
        // This callback is invoked on a per $or branch basis. The callback is constructed in the
        // "sometimes cache" mode. We currently do not support cached plan replanning for rooted $or
        // queries. Therefore, we must be more conservative about putting a potentially bad plan
        // into the cache in the subplan path.
        //
        // TODO SERVER-18777: Support replanning for rooted $or queries.
        .onPickPlanForBranch =
            plan_cache_util::ConditionalClassicPlanCacheWriter{
                plan_cache_util::CacheMode::SometimesCache,
                opCtx(),
                collections().getMainCollectionPtrOrAcquisition()},

        .onPickPlanWholeQuery =
            plan_cache_util::ClassicPlanCacheWriter{
                opCtx(), collections().getMainCollectionPtrOrAcquisition()},
    };

    auto root = std::make_unique<SubplanStage>(cq()->getExpCtxRaw(),
                                               collections().getMainCollectionPtrOrAcquisition(),
                                               ws(),
                                               cq(),
                                               std::move(callbacks));
    _subplanStage = root.get();
    setRoot(std::move(root));
}

Status SubPlanner::doPlan(PlanYieldPolicy* planYieldPolicy) {
    return _subplanStage->pickBestPlan(plannerParams(), planYieldPolicy);
}

std::unique_ptr<QuerySolution> SubPlanner::extractQuerySolution() {
    // This function is called when the plan executor is created to extract the winning plan from
    // the planner, so if this code runs then we know the subplanner was invoked and chose the
    // winning plan.
    incrementClassicSubplannerChoseWinningPlan();
    return nullptr;
}

const QuerySolution* SubPlanner::querySolution() const {
    // Currently this function is only called during replanning to check if the new plan's solution
    // hash is the same as the cached plan's hash. However, subplanning during replanning is
    // currently blocked (SERVER-120492), so this function should never be reached.
    // TODO SERVER-120492: If we can do subplanning within replanning, we should instead return the
    // whole-query QSN (if there is one) from the SubplanStage.
    MONGO_UNREACHABLE_TASSERT(8746606);
}
}  // namespace classic_runtime_planner
}  // namespace mongo
