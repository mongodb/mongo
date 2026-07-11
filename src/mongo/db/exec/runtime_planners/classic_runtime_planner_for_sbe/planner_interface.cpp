// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"

#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

namespace mongo::classic_runtime_planner_for_sbe {

PlannerBase::PlannerBase(PlannerDataForSBE plannerData) : _plannerData(std::move(plannerData)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> PlannerBase::prepareSbePlanExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    std::unique_ptr<QuerySolution> solution,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> sbePlanAndData,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash,
    std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage) {
    const auto* expCtx = cq()->getExpCtxRaw();
    auto remoteCursors =
        expCtx->getExplain() ? nullptr : search_helpers::getSearchRemoteCursors(cq()->cqPipeline());
    auto remoteExplains = expCtx->getExplain()
        ? search_helpers::getSearchRemoteExplains(expCtx, cq()->cqPipeline())
        : nullptr;

    stage_builder::prepareSlotBasedExecutableTree(opCtx(),
                                                  sbePlanAndData.first.get(),
                                                  &sbePlanAndData.second,
                                                  *cq(),
                                                  collections(),
                                                  sbeYieldPolicy(),
                                                  isFromPlanCache,
                                                  remoteCursors.get());

    auto nss = cq()->nss();
    tassert(8551900,
            "Solution must be present if cachedPlanHash is present: ",
            solution != nullptr || !cachedPlanHash.has_value());
    return plan_executor_factory::make(opCtx(),
                                       std::move(canonicalQuery),
                                       std::move(solution),
                                       std::move(sbePlanAndData),
                                       collections(),
                                       plannerOptions(),
                                       std::move(nss),
                                       extractSbeYieldPolicy(),
                                       isFromPlanCache,
                                       cachedPlanHash,
                                       false /*usedJoinOpt*/,
                                       {} /* estimates */,
                                       {} /* rejectedJoinPlans */,
                                       std::move(remoteCursors),
                                       std::move(remoteExplains),
                                       std::move(classicRuntimePlannerStage));
}

std::unique_ptr<QuerySolution> PlannerBase::extendSolutionWithPipeline(
    std::unique_ptr<QuerySolution> solution) {
    if (cq()->cqPipeline().empty()) {
        return solution;
    }
    return QueryPlanner::extendWithAggPipeline(
        *cq(), std::move(solution), plannerParams().secondaryCollectionsInfo);
}

std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
PlannerBase::prepareSbePlanAndData(const QuerySolution& solution,
                                   boost::optional<std::string> replanReason) {
    auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
        opCtx(), collections(), *cq(), solution, sbeYieldPolicy());
    sbePlanAndData.second.replanReason = std::move(replanReason);
    return sbePlanAndData;
}
}  // namespace mongo::classic_runtime_planner_for_sbe
