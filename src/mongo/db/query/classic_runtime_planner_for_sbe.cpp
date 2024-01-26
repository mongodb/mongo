/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/classic_runtime_planner_for_sbe.h"

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

PlannerBase::PlannerBase(OperationContext* opCtx, PlannerData plannerData)
    : _opCtx(opCtx), _plannerData(std::move(plannerData)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> PlannerBase::prepareSbePlanExecutor(
    std::unique_ptr<QuerySolution> solution,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> sbePlanAndData,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash) {
    const auto* expCtx = cq()->getExpCtxRaw();
    auto remoteCursors =
        expCtx->explain ? nullptr : search_helpers::getSearchRemoteCursors(cq()->cqPipeline());
    auto remoteExplains = expCtx->explain
        ? search_helpers::getSearchRemoteExplains(expCtx, cq()->cqPipeline())
        : nullptr;

    stage_builder::prepareSlotBasedExecutableTree(_opCtx,
                                                  sbePlanAndData.first.get(),
                                                  &sbePlanAndData.second,
                                                  *cq(),
                                                  collections(),
                                                  sbeYieldPolicy(),
                                                  isFromPlanCache,
                                                  remoteCursors.get());

    auto nss = cq()->nss();
    tassert(8551900,
            "Solution must be present if cachedPlanHash is present",
            solution != nullptr || !cachedPlanHash.has_value());
    bool matchesCachedPlan = cachedPlanHash && *cachedPlanHash == solution->hash();
    return uassertStatusOK(
        plan_executor_factory::make(_opCtx,
                                    extractCq(),
                                    nullptr /* pipeline - It is not nullptr only in Bonsai */,
                                    std::move(solution),
                                    std::move(sbePlanAndData),
                                    nullptr /* optimizerData - used for Bonsai */,
                                    plannerOptions(),
                                    std::move(nss),
                                    extractSbeYieldPolicy(),
                                    isFromPlanCache,
                                    matchesCachedPlan,
                                    false /* generatedByBonsai */,
                                    OptimizerCounterInfo{} /* used for Bonsai */,
                                    std::move(remoteCursors),
                                    std::move(remoteExplains)));
}

SingleSolutionPassthroughPlanner::SingleSolutionPassthroughPlanner(
    OperationContext* opCtx, PlannerData plannerData, std::unique_ptr<QuerySolution> solution)
    : PlannerBase(opCtx, std::move(plannerData)), _solution(std::move(solution)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> SingleSolutionPassthroughPlanner::plan() {
    LOGV2_DEBUG(8523405, 5, "Using SBE single solution planner");

    if (!cq()->cqPipeline().empty()) {
        _solution = QueryPlanner::extendWithAggPipeline(
            *cq(),
            std::move(_solution),
            fillOutSecondaryCollectionsInformation(opCtx(), collections(), cq()));
    }

    auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
        opCtx(), collections(), *cq(), *_solution, sbeYieldPolicy());

    // Create a pinned plan cache entry.
    plan_cache_util::updatePlanCache(
        opCtx(), collections(), *cq(), *_solution, *sbePlanAndData.first, sbePlanAndData.second);

    return prepareSbePlanExecutor(std::move(_solution),
                                  std::move(sbePlanAndData),
                                  false /*isFromPlanCache*/,
                                  cachedPlanHash());
}

CachedPlanner::CachedPlanner(OperationContext* opCtx,
                             PlannerData plannerData,
                             std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder)
    : PlannerBase(opCtx, std::move(plannerData)), _cachedPlanHolder(std::move(cachedPlanHolder)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> CachedPlanner::plan() {
    LOGV2_DEBUG(8523404, 5, "Recovering SBE plan from the cache");
    return prepareSbePlanExecutor(nullptr /*solution*/,
                                  {std::move(_cachedPlanHolder->cachedPlan->root),
                                   std::move(_cachedPlanHolder->cachedPlan->planStageData)},
                                  true /*isFromPlanCache*/,
                                  boost::none /*cachedPlanHash*/);
}

}  // namespace mongo::classic_runtime_planner_for_sbe
