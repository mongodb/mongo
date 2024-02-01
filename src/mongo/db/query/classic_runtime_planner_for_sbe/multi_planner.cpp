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

#include "mongo/db/query/classic_runtime_planner_for_sbe/planner_interface.h"

#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

MultiPlanner::MultiPlanner(OperationContext* opCtx,
                           PlannerData plannerData,
                           PlanYieldPolicy::YieldPolicy yieldPolicy,
                           std::vector<std::unique_ptr<QuerySolution>> candidatePlans,
                           PlanCachingMode cachingMode)
    : PlannerBase(opCtx, std::move(plannerData)),
      _yieldPolicy(yieldPolicy),
      _cachingMode(cachingMode) {
    _multiPlanStage =
        std::make_unique<MultiPlanStage>(cq()->getExpCtxRaw(),
                                         collections().getMainCollectionPtrOrAcquisition(),
                                         cq(),
                                         PlanCachingMode::NeverCache);
    for (auto&& solution : candidatePlans) {
        auto nextPlanRoot = stage_builder::buildClassicExecutableTree(
            opCtx, collections().getMainCollectionPtrOrAcquisition(), *cq(), *solution, ws());
        _multiPlanStage->addPlan(std::move(solution), std::move(nextPlanRoot), ws());
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> MultiPlanner::plan() {
    LOGV2_DEBUG(6215001, 5, "Using classic multi-planner for SBE");

    auto trialPeriodYieldPolicy =
        makeClassicYieldPolicy(opCtx(),
                               cq()->nss(),
                               static_cast<PlanStage*>(_multiPlanStage.get()),
                               _yieldPolicy,
                               collections().getMainCollectionPtrOrAcquisition());

    // TODO SERVER-85248: Update the sbe plan cache after best plan is chosen.
    uassertStatusOK(_multiPlanStage->pickBestPlan(trialPeriodYieldPolicy.get()));

    std::unique_ptr<QuerySolution> winningSolution = _multiPlanStage->extractBestSolution();

    // Extend the winning solution with the agg pipeline and build the execution tree.
    if (!cq()->cqPipeline().empty()) {
        winningSolution = QueryPlanner::extendWithAggPipeline(
            *cq(),
            std::move(winningSolution),
            fillOutSecondaryCollectionsInformation(opCtx(), collections(), cq()));
    }

    auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
        opCtx(), collections(), *cq(), *winningSolution, sbeYieldPolicy());

    return prepareSbePlanExecutor(std::move(winningSolution),
                                  std::move(sbePlanAndData),
                                  false /*isFromPlanCache*/,
                                  cachedPlanHash());
}
}  // namespace mongo::classic_runtime_planner_for_sbe
