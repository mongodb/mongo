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

#include "mongo/db/query/get_executor_helpers.h"

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
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_utils.h"
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


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {
/**
 * Aggregation of the total number of times query planning occurred.
 */
auto& plannerInvocationCount = *MetricBuilder<Counter64>{"query.planning.invocationCount"};

/**
 * Aggregation of the total number of times the classic subplanner chose the winning plan.
 */
auto& classicChoseWinningPlan =
    *MetricBuilder<Counter64>{"query.subPlanner.classicChoseWinningPlan"};

void inspectPlannerResult(
    const std::unique_ptr<PlannerInterface>& result,
    const boost::optional<QueryPlannerParams::ReplanningData>& replanningData) {
    // These assertions relate to replanning, so bail if the query did not replan.
    // Also, these assertions do not apply to the deferred get_executor. The solution hash is
    // checked after the plan ranking result is created, to update
    // `replannedPlanIsCachedPlanCounter`.
    if (!replanningData.has_value() ||
        feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled()) {
        return;
    }

    // Only planning for classic plans can throw the ReplanningRequired exception. Thus when
    // we call makePlanner again, we should still end up with a ClassicRuntimePlannerResult
    // since the query hasn't changed.
    // TODO SERVER-120501: Throw/catch this exception for SBE plans too.
    auto planner = dynamic_cast<classic_runtime_planner::ClassicPlannerInterface*>(result.get());
    tassert(8746607,
            "Replanning should have resulted in a ClassicPlannerInterface, but it didn't",
            planner);

    // We do not consult the plan cache upon replanning so we should never enounter a
    // CachedPlanner here.
    tassert(8746608,
            "Replanning should not have resulted in a CachedPlanner, but it did",
            !dynamic_cast<classic_runtime_planner::CachedPlanner*>(planner));

    // TODO SERVER-120492: Investigate if we can remove this tassert.
    tassert(8746609,
            "Replanning should not have resulted in a SubPlanner, but it did",
            !dynamic_cast<classic_runtime_planner::SubPlanner*>(planner));

    const QuerySolution* solution = planner->querySolution();
    if (!solution) {
        // Solution may live in the MultiPlanStage.
        // TODO SERVER-117118 This should not be needed once we decouple the MultiPlanStage.
        if (auto* mps = dynamic_cast<MultiPlanStage*>(planner->getRoot())) {
            solution = mps->bestSolution();
        }
    }
    const auto isSameAsCachedPlan = solution && replanningData->oldPlanHash == solution->hash();

    LOGV2_DEBUG(
        20582,
        1,
        "Query plan after replanning and its cache status",
        "query"_attr = redact(planner->cq()->toStringShort()),
        "planSummary"_attr = plan_explainer_factory::make(planner->getRoot())->getPlanSummary(),
        "shouldCache"_attr =
            ((replanningData->shouldCache == plan_cache_util::CacheMode::AlwaysCache) ? "yes"
                                                                                      : "no"),
        "isSameAsCachedPlan"_attr = isSameAsCachedPlan);

    if (isSameAsCachedPlan) {
        planCacheCounters.incrementClassicReplannedPlanIsCachedPlanCounter();
    }
}
}  // namespace

void setOpDebugPlanCacheInfo(OperationContext* opCtx, const PlanCacheInfo& cacheInfo) {
    OpDebug& opDebug = CurOp::get(opCtx)->debug();
    if (!opDebug.planCacheShapeHash && cacheInfo.planCacheShapeHash) {
        opDebug.planCacheShapeHash = *cacheInfo.planCacheShapeHash;
    }
    if (!opDebug.planCacheKey && cacheInfo.planCacheKey) {
        opDebug.planCacheKey = *cacheInfo.planCacheKey;
    }
}

void incrementPlannerInvocationCount() {
    plannerInvocationCount.increment();
}

void incrementClassicSubplannerChoseWinningPlan() {
    classicChoseWinningPlan.increment();
}

std::unique_ptr<PlannerInterface> retryMakePlanner(
    std::unique_ptr<QueryPlannerParams> plannerParams,
    const MakePlannerParamsFn& makeQueryPlannerParams,
    const MakePlannerFn& makePlanner,
    CanonicalQuery* canonicalQuery,
    std::size_t plannerOptions,
    Pipeline* pipeline) {
    // We create this once on replanning and then make a copy for each QueryPlannerParams to own for
    // subsequent calls.
    boost::optional<QueryPlannerParams::ReplanningData> replanningData = boost::none;

    static constexpr size_t kMaxIterations = 5;
    for (size_t iter = 0; iter < kMaxIterations; ++iter) {
        try {
            // First try the single collection query parameters, as these would have been
            // generated with query settings if present.
            auto result = makePlanner(std::move(plannerParams));
            inspectPlannerResult(result, replanningData);
            return result;
        } catch (const ExceptionFor<ErrorCodes::NoDistinctScansForDistinctEligibleQuery>&) {
            // The planner failed to generate a DISTINCT_SCAN for a distinct-like query. Remove
            // the distinct property and replan using SBE or subplanning as applicable.
            canonicalQuery->resetDistinct();
            if (canonicalQuery->isSbeCompatible() &&
                !feature_flags::gFeatureFlagGetExecutorDeferredEngineChoice.isEnabled()) {
                // Stages still need to be finalized for SBE since classic was used previously. In
                // the deferred get_executor, the stages are finalized during lowering.
                finalizePipelineStages(pipeline, canonicalQuery);
            }
            return makePlanner(
                makeQueryPlannerParams(*canonicalQuery, plannerOptions, replanningData));
        } catch (const ExceptionFor<ErrorCodes::NoQueryExecutionPlans>& exception) {
            // The planner failed to generate a viable plan. Remove the query settings and
            // retry if any are present. Otherwise just propagate the exception.
            const auto& querySettings = canonicalQuery->getExpCtx()->getQuerySettings();
            const bool hasQuerySettings = querySettings.getIndexHints().has_value();
            // Planning has been tried without query settings and no execution plan was found.
            const bool ignoreQuerySettings =
                plannerOptions & QueryPlannerParams::IGNORE_QUERY_SETTINGS;
            if (!hasQuerySettings || ignoreQuerySettings) {
                throw;
            }
            LOGV2_DEBUG(8524200,
                        2,
                        "Encountered planning error while running with query settings. Retrying "
                        "without query settings.",
                        "query"_attr = redact(canonicalQuery->toStringForErrorMsg()),
                        "querySettings"_attr = querySettings,
                        "reason"_attr = exception.reason(),
                        "code"_attr = exception.codeString());
            CurOp::get(canonicalQuery->getExpCtx()->getOperationContext())
                ->debug()
                .failedPlanningWithQuerySettings = true;
            plannerOptions |= QueryPlannerParams::IGNORE_QUERY_SETTINGS;
            // Propagate the params to the next iteration.
            plannerParams = makeQueryPlannerParams(*canonicalQuery, plannerOptions, replanningData);
        } catch (const ExceptionFor<ErrorCodes::RetryMultiPlanning>&) {
            // Propagate the params to the next iteration.
            plannerParams = makeQueryPlannerParams(*canonicalQuery, plannerOptions, replanningData);
            canonicalQuery->getExpCtx()->setWasRateLimited(true);
        } catch (const ExceptionFor<ErrorCodes::ReplanningRequired>& exception) {
            // Propagate the params and replanning information to the next iteration.
            auto replanningInfo = exception.extraInfo<ReplanningRequiredInfo>();
            replanningData = boost::make_optional<QueryPlannerParams::ReplanningData>(
                {.replanReason = std::move(exception.reason()),
                 .shouldCache = replanningInfo->getCacheMode(),
                 .oldPlanHash = replanningInfo->getOldPlanHash()});
            plannerParams = makeQueryPlannerParams(*canonicalQuery, plannerOptions, replanningData);
        }
    }
    tasserted(8712800, "Exceeded retry iterations for making a planner");
}

bool shouldMultiPlanForSingleSolution(const PlanRankingResult& rankerResult,
                                      const CanonicalQuery* cq) {
    auto expCtx = cq->getExpCtxRaw();

    // Force multiplanning (and therefore caching) if forcePlanCache is set. We could
    // manually update the plan cache instead without multiplanning but this is simpler.
    bool forceMultiPlanForSingleSolution = expCtx->getForcePlanCache() ||
        expCtx->getQueryKnobConfiguration().getUseMultiplannerForSingleSolutions();

    // If we have reached this point and 'needsWorksMeasuredForPlanCache' is true, then there
    // has been no multiplanning work done so far and we will run the single CBR-chosen solution
    // through the multiplanner to measure its number of works and add the plan to the plan
    // cache. If 'internalQueryDisablePlanCache' disables the plan cache, we will ignore
    // 'needsWorksMeasuredForPlanCache' and instead only check whether we should force running the
    // single solution plan through the multiplanner.
    return (!internalQueryDisablePlanCache.load() && rankerResult.needsWorksMeasuredForPlanCache) ||
        forceMultiPlanForSingleSolution;
}

void captureCardinalityEstimationMethodForQueryStats(
    OperationContext* opCtx,
    const boost::optional<PlanExplainerData>& maybeExplainData,
    const QuerySolution* solution) {
    if (maybeExplainData && !maybeExplainData->estimates.empty() && solution) {
        auto it = maybeExplainData->estimates.find(solution->root());
        if (it != maybeExplainData->estimates.end()) {
            auto& ceMethods =
                CurOp::get(opCtx)->debug().getAdditiveMetrics().cardinalityEstimationMethods;
            switch (it->second->outCE.source()) {
                case cost_based_ranker::EstimationSource::Histogram:
                    ceMethods.setHistogram(ceMethods.getHistogram().value_or(0) + 1);
                    break;
                case cost_based_ranker::EstimationSource::Sampling:
                    ceMethods.setSampling(ceMethods.getSampling().value_or(0) + 1);
                    break;
                case cost_based_ranker::EstimationSource::Heuristics:
                    ceMethods.setHeuristics(ceMethods.getHeuristics().value_or(0) + 1);
                    break;
                case cost_based_ranker::EstimationSource::Mixed:
                    ceMethods.setMixed(ceMethods.getMixed().value_or(0) + 1);
                    break;
                case cost_based_ranker::EstimationSource::Metadata:
                    ceMethods.setMetadata(ceMethods.getMetadata().value_or(0) + 1);
                    break;
                case cost_based_ranker::EstimationSource::Code:
                    ceMethods.setCode(ceMethods.getCode().value_or(0) + 1);
                    break;
                default:
                    MONGO_UNREACHABLE_TASSERT(11560601);
                    break;
            }
        }
    }
}

}  // namespace mongo
