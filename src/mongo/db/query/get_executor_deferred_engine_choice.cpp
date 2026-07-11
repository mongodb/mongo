// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/get_executor_deferred_engine_choice_lowering.h"
#include "mongo/db/query/get_executor_deferred_engine_choice_planning.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_driver.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec_deferred_engine_choice {

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getExecutorFindDeferredEngineChoice(OperationContext* opCtx,
                                    const MultipleCollectionAccessor& collections,
                                    std::unique_ptr<CanonicalQuery> canonicalQuery,
                                    PlanYieldPolicy::YieldPolicy yieldPolicy,
                                    const MakePlannerParamsFn& makeQueryPlannerParams,
                                    std::size_t plannerOptions,
                                    Pipeline* pipeline) {
    PlanRankingResult rankerResult = planRanking(opCtx,
                                                 collections,
                                                 canonicalQuery,
                                                 yieldPolicy,
                                                 plannerOptions,
                                                 pipeline,
                                                 makeQueryPlannerParams);
    if (rankerResult.expressExecutor) {
        return {std::move(rankerResult.expressExecutor)};
    }
    // If we replanned and the old plan and new plan are the same, update the counter.
    const auto replanningData = rankerResult.plannerParams->replanningData;
    if (replanningData && !rankerResult.solutions.empty()) {
        const auto qsn = rankerResult.solutions.at(0).get();
        const bool isSameAsCachedPlan = qsn && replanningData->oldPlanHash == qsn->hash();
        if (isSameAsCachedPlan) {
            planCacheCounters.incrementClassicReplannedPlanIsCachedPlanCounter();
        }
        // On the inside of a $lookup, a single solution may be cached. This means when the inner
        // query is replanned, there won't be any exec state because execution was not needed to
        // determine the winning plan.
        if (rankerResult.execState) {
            const auto classicExecStats = rankerResult.execState->peekExecState<ClassicExecState>();
            tassert(12870802,
                    "Expected classic execState to exist after replanning.",
                    classicExecStats);
            LOGV2_DEBUG(
                12870800,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(canonicalQuery->toStringShort()),
                "planSummary"_attr =
                    plan_explainer_factory::make(classicExecStats->root.get())->getPlanSummary(),
                "shouldCache"_attr =
                    replanningData->shouldCache == plan_cache_util::CacheMode::AlwaysCache ? "yes"
                                                                                           : "no",
                "isSameAsCachedPlan"_attr = isSameAsCachedPlan);
        }
    }
    return lowerPlanRankingResult(std::move(canonicalQuery),
                                  std::move(rankerResult),
                                  opCtx,
                                  collections,
                                  yieldPolicy,
                                  pipeline);
}

}  // namespace mongo::exec_deferred_engine_choice
