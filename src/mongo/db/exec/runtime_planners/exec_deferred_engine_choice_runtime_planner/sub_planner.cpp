/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/plan_yield_policy_impl.h"

namespace mongo::exec_deferred_engine_choice {

SubPlanner::SubPlanner(PlannerData plannerData)
    : DeferredEngineChoicePlannerInterface(std::move(plannerData)) {
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
    _subPlanStage =
        std::make_unique<SubplanStage>(cq()->getExpCtxRaw(),
                                       collections().getMainCollectionPtrOrAcquisition(),
                                       ws(),
                                       cq(),
                                       std::move(callbacks));
    auto trialPeriodYieldPolicy = makeClassicYieldPolicy(
        opCtx(), cq()->nss(), static_cast<PlanStage*>(_subPlanStage.get()), yieldPolicy());
    uassertStatusOK(_subPlanStage->pickBestPlan(*plannerParams(), trialPeriodYieldPolicy.get()));
    incrementClassicSubplannerChoseWinningPlan();
}

PlanRankingResult SubPlanner::extractPlanRankingResult() {
    tassert(11974303,
            "Expected `extractPlanRankingResult` to only be called with get executor deferred "
            "feature flag enabled.",
            feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled());
    auto querySolution = _subPlanStage->extractBestWholeQuerySolution();

    return PlanRankingResult{.solutions = makeQsnResult(std::move(querySolution)),
                             .execState = SavedExecState{ClassicExecState{
                                 .workingSet = extractWs(), .root = std::move(_subPlanStage)}},
                             .plannerParams = extractPlannerParams(),
                             .cachedPlanHash = cachedPlanHash()};
}
}  // namespace mongo::exec_deferred_engine_choice
