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

#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

MultiPlanner::MultiPlanner(PlannerDataForSBE plannerData,
                           std::vector<std::unique_ptr<QuerySolution>> candidatePlans,
                           PlanCachingMode cachingMode,
                           boost::optional<std::string> replanReason)
    : PlannerBase(std::move(plannerData)),
      _cachingMode((std::move(cachingMode))),
      _replanReason(replanReason) {
    _multiPlanStage =
        std::make_unique<MultiPlanStage>(cq()->getExpCtxRaw(),
                                         collections().getMainCollectionPtrOrAcquisition(),
                                         cq(),
                                         PlanCachingMode::NeverCache,
                                         replanReason);
    for (auto&& solution : candidatePlans) {
        auto nextPlanRoot = stage_builder::buildClassicExecutableTree(
            opCtx(), collections().getMainCollectionPtrOrAcquisition(), *cq(), *solution, ws());
        _multiPlanStage->addPlan(std::move(solution), std::move(nextPlanRoot), ws());
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> MultiPlanner::plan() {
    LOGV2_DEBUG(6215001, 5, "Using classic multi-planner for SBE");

    auto trialPeriodYieldPolicy =
        makeClassicYieldPolicy(opCtx(),
                               cq()->nss(),
                               static_cast<PlanStage*>(_multiPlanStage.get()),
                               yieldPolicy(),
                               collections().getMainCollectionPtrOrAcquisition());

    uassertStatusOK(_multiPlanStage->pickBestPlan(trialPeriodYieldPolicy.get()));

    // Calculate and update the number of works based on totalKeysExamined + totalDocsExamined to
    // align with how SBE calculates the works.
    auto stats = _multiPlanStage->getStats();
    auto summary = collectExecutionStatsSummary(stats.get(), _multiPlanStage->bestPlanIdx());
    plan_ranker::PlanRankingDecision ranking = _multiPlanStage->planRankingDecision();
    tassert(8523807,
            "Expected StatsDetails in classic runtime planner ranking decision.",
            std::holds_alternative<plan_ranker::StatsDetails>(ranking.stats));
    auto& rankerDetails = std::get<plan_ranker::StatsDetails>(ranking.stats);
    rankerDetails.candidatePlanStats[0]->common.works =
        summary.totalKeysExamined + summary.totalDocsExamined;

    if (_shouldUseEofOptimization()) {
        // We don't need to extend the solution with the agg pipeline, because we don't do EOF
        // optimization if the pipeline is present.
        _buildSbePlanAndUpdatePlanCache(_multiPlanStage->bestSolution(), ranking);
        auto nss = cq()->nss();
        return uassertStatusOK(
            plan_executor_factory::make(extractCq(),
                                        extractWs(),
                                        std::move(_multiPlanStage),
                                        collections().getMainCollectionPtrOrAcquisition(),
                                        yieldPolicy(),
                                        plannerOptions(),
                                        std::move(nss),
                                        nullptr /* querySolution */,
                                        cachedPlanHash()));
    }

    std::unique_ptr<QuerySolution> winningSolution = _multiPlanStage->extractBestSolution();

    // Extend the winning solution with the agg pipeline and build the execution tree.
    if (!cq()->cqPipeline().empty()) {
        winningSolution = QueryPlanner::extendWithAggPipeline(
            *cq(), std::move(winningSolution), plannerParams().secondaryCollectionsInfo);
    }

    auto sbePlanAndData = _buildSbePlanAndUpdatePlanCache(winningSolution.get(), ranking);
    return prepareSbePlanExecutor(std::move(winningSolution),
                                  std::move(sbePlanAndData),
                                  false /*isFromPlanCache*/,
                                  cachedPlanHash(),
                                  std::move(_multiPlanStage));
}

MultiPlanner::SbePlanAndData MultiPlanner::_buildSbePlanAndUpdatePlanCache(
    const QuerySolution* winningSolution, const plan_ranker::PlanRankingDecision& ranking) {
    auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
        opCtx(), collections(), *cq(), *winningSolution, sbeYieldPolicy());
    sbePlanAndData.second.replanReason = std::move(_replanReason);
    plan_cache_util::updateSbePlanCacheFromClassicCandidates(opCtx(),
                                                             collections(),
                                                             _cachingMode,
                                                             *cq(),
                                                             ranking,
                                                             _multiPlanStage->candidates(),
                                                             sbePlanAndData,
                                                             winningSolution);
    return sbePlanAndData;
}

bool MultiPlanner::_shouldUseEofOptimization() const {
    return _multiPlanStage->bestSolutionEof() &&
        // We show SBE plan in explain.
        !cq()->getExpCtxRaw()->explain &&
        // We can't use EOF optimization if pipeline is present. Because we need to execute the
        // pipeline part in SBE, we have to rebuild and rerun the whole query.
        // TODO SERVER-86061 Avoid rerunning find part of the query if it reached EOF during
        // planning.
        cq()->cqPipeline().empty() &&
        // We want more coverage for SBE in debug builds.
        !kDebugBuild;
}

}  // namespace mongo::classic_runtime_planner_for_sbe
