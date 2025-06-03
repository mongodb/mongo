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

#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

SubPlanner::SubPlanner(PlannerDataForSBE plannerData) : PlannerBase(std::move(plannerData)) {
    LOGV2_DEBUG(8542100, 5, "Using classic subplanner for SBE");

    _subplanStage =
        std::make_unique<SubplanStage>(cq()->getExpCtxRaw(),
                                       collections().getMainCollectionPtrOrAcquisition(),
                                       ws(),
                                       cq(),
                                       makeCallbacks());

    auto trialPeriodYieldPolicy =
        makeClassicYieldPolicy(opCtx(),
                               cq()->nss(),
                               static_cast<PlanStage*>(_subplanStage.get()),
                               yieldPolicy(),
                               collections().getMainCollectionPtrOrAcquisition());
    uassertStatusOK(_subplanStage->pickBestPlan(plannerParams(),
                                                trialPeriodYieldPolicy.get(),
                                                false /* shouldConstructClassicExecutableTree */));

    _solution = extendSolutionWithPipeline(_subplanStage->extractBestWholeQuerySolution());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> SubPlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    auto sbePlanAndData = prepareSbePlanAndData(*_solution);

    if (useSbePlanCache()) {
        plan_cache_util::updateSbePlanCacheWithPinnedEntry(opCtx(),
                                                           collections(),
                                                           *cq(),
                                                           *_solution,
                                                           *sbePlanAndData.first.get(),
                                                           sbePlanAndData.second);
    }

    return prepareSbePlanExecutor(std::move(canonicalQuery),
                                  std::move(_solution),
                                  std::move(sbePlanAndData),
                                  false /*isFromPlanCache*/,
                                  cachedPlanHash(),
                                  nullptr /*classicRuntimePlannerStage*/);
}
SubplanStage::PlanSelectionCallbacks SubPlanner::makeCallbacks() {
    if (useSbePlanCache()) {
        // When using the SBE plan cache, pass no-op callbacks to the 'SubplanStage'. We take care
        // of writing the complete composite plan to the SBE plan cache ourselves.
        return SubplanStage::PlanSelectionCallbacks{
            .onPickPlanForBranch =
                [this](const CanonicalQuery&,
                       MultiPlanStage& mps,
                       std::unique_ptr<plan_ranker::PlanRankingDecision>,
                       std::vector<plan_ranker::CandidatePlan>&) { ++_numPerBranchMultiplans; },
            .onPickPlanWholeQuery = plan_cache_util::NoopPlanCacheWriter{},
        };
    } else {
        // This callback is invoked on a per $or branch basis. The callback is constructed in the
        // "sometimes cache" mode. We currently do not support cached plan replanning for rooted $or
        // queries. Therefore, we must be more conservative about putting a potentially bad plan
        // into the cache in the subplan path.
        //
        // TODO SERVER-18777: Support replanning for rooted $or queries.
        plan_cache_util::ConditionalClassicPlanCacheWriter perBranchWriter{
            plan_cache_util::ConditionalClassicPlanCacheWriter::Mode::SometimesCache,
            opCtx(),
            collections().getMainCollectionPtrOrAcquisition(),
            false /* executeInSbe. We set this to false because the cache entry created by this
                     callback is only used by the SubPlanner. It is not used for constructing a
                     final execution plan.  This is marked by a special byte in the plan cache key
                     which indicates that the entry is used for subplanning only.
                   */};

        // Wrap the conditional classic plan cache writer function object so that we can count the
        // number of times that multi-planning gets invoked for an $or branch.
        auto perBranchCallback = [this, capturedPerBranchWriter = std::move(perBranchWriter)](
                                     const CanonicalQuery& cq,
                                     MultiPlanStage& mps,
                                     std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                                     std::vector<plan_ranker::CandidatePlan>& candidates) {
            ++_numPerBranchMultiplans;
            capturedPerBranchWriter(cq, mps, std::move(ranking), candidates);
        };

        // The query will run in SBE but we are using the classic plan cache. Use callbacks to write
        // a classic plan cache entry for each branch.
        return SubplanStage::PlanSelectionCallbacks{
            .onPickPlanForBranch = std::move(perBranchCallback),
            .onPickPlanWholeQuery =
                plan_cache_util::ClassicPlanCacheWriter{
                    opCtx(),
                    collections().getMainCollectionPtrOrAcquisition(),
                    true /* executeInSbe */},
        };
    }
}

}  // namespace mongo::classic_runtime_planner_for_sbe
