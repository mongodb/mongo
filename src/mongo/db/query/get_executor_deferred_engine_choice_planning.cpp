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

#include "mongo/db/query/get_executor_deferred_engine_choice_planning.h"

#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/get_executor_fast_paths.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/stats/counters.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec_deferred_engine_choice {

namespace {
StatusWith<std::unique_ptr<PlannerInterface>> planWithCBR(
    OperationContext* opCtx,
    CanonicalQuery* cq,
    const std::shared_ptr<QueryPlannerParams>& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    boost::optional<size_t> cachedPlanHash) {
    auto makePlannerData = [&]() {
        return PlannerData{opCtx,
                           cq,
                           std::make_unique<WorkingSet>(),
                           collections,
                           plannerParams,
                           yieldPolicy,
                           cachedPlanHash};
    };

    plan_ranking::PlanRanker planRanker;
    // Pass isClassic=true because CBR internally uses classic multiplanning; the actual engine
    // choice (classic vs SBE) is deferred to the lowering phase.
    auto rankerResultSW = planRanker.rankPlans(opCtx,
                                               *cq,
                                               *plannerParams,
                                               yieldPolicy,
                                               collections,
                                               makePlannerData(),
                                               true /* isClassic */);
    if (!rankerResultSW.isOK()) {
        return rankerResultSW.getStatus();
    }
    auto rankerResult = std::move(rankerResultSW.getValue());

    if (!rankerResult.solutions.empty()) {
        captureCardinalityEstimationMethodForQueryStats(
            opCtx, rankerResult.maybeExplainData, rankerResult.solutions[0].get());
    }

    if (rankerResult.execState) {
        // Some CBR strategies use MultiPlanner internally and they keep QuerySolution owned by
        // MultiPlanStage. Extract the actual winning solution from the MultiPlanStage so downstream
        // code can use it.
        if (!rankerResult.solutions.empty() && rankerResult.solutions[0] == nullptr) {
            auto* mps = dynamic_cast<MultiPlanStage*>(rankerResult.execState->root.get());
            tassert(11960601, "Expected MultiPlanStage in execState when solution is null", mps);
            rankerResult.solutions[0] = mps->extractBestSolution();
        }
        rankerResult.plannerParams = plannerParams;
        rankerResult.cachedPlanHash = cachedPlanHash;
        return std::make_unique<PreComputedRankingResultPlanner>(makePlannerData(),
                                                                 std::move(rankerResult));
    }

    tassert(11960600,
            "Expected at least one solution from plan ranking",
            !rankerResult.solutions.empty());

    if (rankerResult.solutions.size() == 1 && !shouldMultiPlanForSingleSolution(rankerResult, cq)) {
        rankerResult.solutions[0]->indexFilterApplied = plannerParams->indexFiltersApplied;
        return std::make_unique<SingleSolutionPassthroughPlanner>(
            makePlannerData(),
            std::move(rankerResult.solutions[0]),
            std::move(rankerResult.maybeExplainData));
    }

    return std::make_unique<MultiPlanner>(makePlannerData(),
                                          std::move(rankerResult.solutions),
                                          rankerResult.needsWorksMeasuredForPlanCache,
                                          std::move(rankerResult.maybeExplainData));
}
}  // namespace

StatusWith<std::unique_ptr<PlannerInterface>> preparePlanner(
    OperationContext* opCtx,
    CanonicalQuery* cq,
    std::shared_ptr<QueryPlannerParams> plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    Pipeline* pipeline) {
    auto makePlannerData = [&](boost::optional<size_t> cachedPlanHash) {
        return PlannerData{opCtx,
                           cq,
                           std::make_unique<WorkingSet>(),
                           collections,
                           plannerParams,
                           yieldPolicy,
                           cachedPlanHash};
    };
    auto buildSingleSolutionPlanner = [&](std::unique_ptr<QuerySolution> solution,
                                          boost::optional<size_t> cachedPlanHash) {
        return std::make_unique<SingleSolutionPassthroughPlanner>(makePlannerData(cachedPlanHash),
                                                                  std::move(solution));
    };

    const auto& mainColl = collections.getMainCollection();
    if (!mainColl) {
        LOGV2_DEBUG(11742304,
                    2,
                    "Collection does not exist. Using EOF plan",
                    logAttrs(cq->nss()),
                    "canonicalQuery"_attr = redact(cq->toStringShort()));
        planCacheCounters.incrementClassicSkippedCounter();
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::make_unique<EofNode>(eof_node::EOFType::NonExistentNamespace));
        return buildSingleSolutionPlanner(std::move(solution), boost::none /*cachedPlanHash*/);
    }

    if (cq->getFindCommandRequest().getTailable() && !mainColl->isCapped()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "error processing query: " << cq->toStringForErrorMsg()
                                    << " tailable cursor requested on non capped collection");
    }

    // If the canonical query does not have a user-specified collation and no one has given the
    // CanonicalQuery a collation already, set it from the collection default.
    if (cq->getFindCommandRequest().getCollation().isEmpty() && cq->getCollator() == nullptr &&
        mainColl->getDefaultCollator()) {
        cq->setCollator(mainColl->getDefaultCollator()->clone());
    }

    if (auto idHackPlan =
            tryIdHack(opCtx,
                      collections,
                      cq,
                      [&]() { return makePlannerData(boost::none /*cachedPlanHash*/); });
        idHackPlan) {
        uassertStatusOK(idHackPlan->plan());
        return {{std::move(idHackPlan)}};
    }

    auto planCacheKey =
        plan_cache_key_factory::make<PlanCacheKey>(*cq, collections.getMainCollectionAcquisition());
    PlanCacheInfo planCacheInfo{planCacheKey.planCacheKeyHash(), planCacheKey.planCacheShapeHash()};
    setOpDebugPlanCacheInfo(opCtx, planCacheInfo);

    boost::optional<size_t> cachedPlanHash = boost::none;
    if (auto cs = CollectionQueryInfo::get(collections.getMainCollection())
                      .getPlanCache()
                      ->getCacheEntryIfActive(planCacheKey)) {
        // TODO SERVER-117566 Implement plan cache lookup, including failpoint for forced replanning
        // to get additional cache + replanning coverage.
        cachedPlanHash = cs->cachedPlan->solutionHash;
    }

    if (SubplanStage::needsSubplanning(*cq)) {
        return std::make_unique<SubPlanner>(makePlannerData(cachedPlanHash));
    }

    if (plannerParams->cbrEnabled) {
        return planWithCBR(opCtx, cq, plannerParams, yieldPolicy, collections, cachedPlanHash);
    }

    auto solutions = uassertStatusOK(QueryPlanner::plan(*cq, *plannerParams));
    // The planner should have returned an error status if there are no solutions.
    tassert(11742305, "Expected at least one solution to answer query", !solutions.empty());

    // If there is a single solution, we can return that plan.
    // Force multiplanning (and therefore caching) if forcePlanCache is set. We could
    // manually update the plan cache instead without multiplanning but this is simpler.
    if (1 == solutions.size() && !cq->getExpCtxRaw()->getForcePlanCache() &&
        !cq->getExpCtxRaw()->getQueryKnobConfiguration().getUseMultiplannerForSingleSolutions()) {
        // Only one possible plan. Build the stages from the solution.
        solutions[0]->indexFilterApplied = plannerParams->indexFiltersApplied;
        return buildSingleSolutionPlanner(std::move(solutions[0]), cachedPlanHash);
    }
    return std::make_unique<MultiPlanner>(makePlannerData(cachedPlanHash), std::move(solutions));
}

PlanRankingResult planRanking(OperationContext* opCtx,
                              const MultipleCollectionAccessor& collections,
                              std::unique_ptr<CanonicalQuery>& canonicalQuery,
                              PlanYieldPolicy::YieldPolicy yieldPolicy,
                              std::size_t plannerOptions,
                              Pipeline* pipeline,
                              const MakePlannerParamsFn& makeQueryPlannerParams) {
    auto expressResult =
        tryExpress(opCtx, collections, canonicalQuery, plannerOptions, makeQueryPlannerParams);
    if (expressResult.executor) {
        return PlanRankingResult{.expressExecutor = std::move(expressResult.executor)};
    }
    // If no express executor was returned, we can reuse the planner params created by `tryExpress`
    // for other planning logic.
    tassert(11974306, "Expected planner params to be initialized.", expressResult.plannerParams);
    incrementPlannerInvocationCount();
    auto paramsForSingleCollectionQuery = std::move(expressResult.plannerParams);

    auto makePlannerHelper = [&](std::unique_ptr<QueryPlannerParams> plannerParams,
                                 boost::optional<std::string> replanReason = boost::none,
                                 boost::optional<bool> shouldCache = boost::none) {
        return uassertStatusOK(preparePlanner(opCtx,
                                              canonicalQuery.get(),
                                              std::move(plannerParams),
                                              yieldPolicy,
                                              collections,
                                              pipeline));
    };
    canonicalQuery->setUsingSbePlanCache(false);
    return retryMakePlanner(std::move(paramsForSingleCollectionQuery),
                            makeQueryPlannerParams,
                            makePlannerHelper,
                            canonicalQuery.get(),
                            plannerOptions,
                            pipeline)
        ->extractPlanRankingResult();
}

}  // namespace mongo::exec_deferred_engine_choice
