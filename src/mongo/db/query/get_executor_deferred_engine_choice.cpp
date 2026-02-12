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

#include "mongo/db/query/get_executor_deferred_engine_choice.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor_fast_paths.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec_deferred_engine_choice {

StatusWith<std::unique_ptr<PlannerInterface>> preparePlanner(
    OperationContext* opCtx,
    CanonicalQuery* cq,
    std::shared_ptr<QueryPlannerParams> plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    Pipeline* pipeline) {
    auto ws = std::make_unique<WorkingSet>();
    auto makePlannerData = [&]() {
        return PlannerData{
            opCtx, cq, std::move(ws), collections, plannerParams, yieldPolicy, boost::none};
    };
    auto buildSingleSolutionPlanner = [&](std::unique_ptr<QuerySolution> solution) {
        return std::make_unique<SingleSolutionPassthroughPlanner>(makePlannerData(),
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
        return buildSingleSolutionPlanner(std::move(solution));
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

    if (auto idHackPlan = tryIdHack(opCtx, collections, cq, makePlannerData); idHackPlan) {
        uassertStatusOK(idHackPlan->plan());
        return {{std::move(idHackPlan)}};
    }

    // TODO SERVER-117566 Implement plan cache storing and lookup.
    auto planCacheKey =
        plan_cache_key_factory::make<PlanCacheKey>(*cq, collections.getMainCollectionAcquisition());
    PlanCacheInfo planCacheInfo{planCacheKey.planCacheKeyHash(), planCacheKey.planCacheShapeHash()};
    setOpDebugPlanCacheInfo(opCtx, planCacheInfo);

    if (SubplanStage::needsSubplanning(*cq)) {
        return std::make_unique<SubPlanner>(makePlannerData());
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
        return buildSingleSolutionPlanner(std::move(solutions[0]));
    }
    return std::make_unique<MultiPlanner>(makePlannerData(), std::move(solutions));
}

std::unique_ptr<PlannerInterface> getPlanner(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* canonicalQuery,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    std::size_t plannerOptions,
    Pipeline* pipeline,
    const MakePlannerParamsFn& makeQueryPlannerParams,
    std::unique_ptr<QueryPlannerParams> paramsForSingleCollectionQuery) {
    auto makePlannerHelper = [&](std::unique_ptr<QueryPlannerParams> plannerParams) {
        return uassertStatusOK(preparePlanner(
            opCtx, canonicalQuery, std::move(plannerParams), yieldPolicy, collections, pipeline));
    };
    canonicalQuery->setUsingSbePlanCache(false);
    return retryMakePlanner(std::move(paramsForSingleCollectionQuery),
                            makeQueryPlannerParams,
                            makePlannerHelper,
                            canonicalQuery,
                            plannerOptions,
                            pipeline);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getExecutorFindDeferredEngineChoice(OperationContext* opCtx,
                                    const MultipleCollectionAccessor& collections,
                                    std::unique_ptr<CanonicalQuery> canonicalQuery,
                                    PlanYieldPolicy::YieldPolicy yieldPolicy,
                                    const MakePlannerParamsFn& makeQueryPlannerParams,
                                    std::size_t plannerOptions,
                                    Pipeline* pipeline) {
    auto expressResult =
        tryExpress(opCtx, collections, canonicalQuery, plannerOptions, makeQueryPlannerParams);
    if (expressResult.executor) {
        return std::move(expressResult.executor);
    }

    auto planner = getPlanner(opCtx,
                              collections,
                              canonicalQuery.get(),
                              yieldPolicy,
                              plannerOptions,
                              pipeline,
                              makeQueryPlannerParams,
                              // Reuse planner params created during failed express path check.
                              std::move(expressResult.plannerParams));
    return planner->makeExecutor(std::move(canonicalQuery), pipeline);
}

}  // namespace mongo::exec_deferred_engine_choice
