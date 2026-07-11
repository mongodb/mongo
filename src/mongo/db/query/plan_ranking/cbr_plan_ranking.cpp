// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"

#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/plan_ranking/plan_ranker_method.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
/**
 * Aggregation of the total number of microseconds spent in CBR.
 * This includes both sampling and ranking time.
 */
auto& microsTotal = *MetricBuilder<DurationCounter64<Microseconds>>{"query.cbr.micros"};

/**
 * Aggregation of the total number of microseconds spent sampling in CBR.
 */
auto& samplingMicrosTotal =
    *MetricBuilder<DurationCounter64<Microseconds>>{"query.cbr.samplingMicros"};

/**
 * Aggregation of the total number of invocations of CBR.
 */
auto& invocationCount = *MetricBuilder<Counter64>{"query.cbr.count"};

/**
 * Aggregation of the total number of candidate plans.
 */
auto& numPlansTotal = *MetricBuilder<Counter64>{"query.cbr.numPlans"};

/**
 * An element in this histogram is the number of microseconds spent in an invocation of CBR.
 * This includes both sampling and ranking time.
 */
auto& microsHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.cbr.histograms.micros"}.bind(
        HistogramServerStatusMetric::pow(11, 1024, 4));

/**
 * An element in this histogram is the number of microseconds spent sampling in an invocation of
 * CBR.
 */
auto& samplingMicrosHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.cbr.histograms.samplingMicros"}.bind(
        HistogramServerStatusMetric::pow(11, 1024, 4));

/**
 * An element in this histogram is the number of plans in the candidate set of an invocation of CBR.
 */
auto& numPlansHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.cbr.histograms.numPlans"}.bind(
        HistogramServerStatusMetric::pow(5, 2, 2));

}  // namespace

namespace {
bool isTriviallyEstimable(const CanonicalQuery& cq) {
    const auto pme = cq.getPrimaryMatchExpression();
    return pme->isTriviallyTrue() || pme->isTriviallyFalse();
}
}  // namespace

namespace plan_ranking {

StatusWith<PlanRankingResult> CBRPlanRankingStrategy::rankPlans(PlannerData& pd,
                                                                RankingContext& rctx) {
    return rankPlans(pd.opCtx,
                     *pd.cq,
                     *pd.plannerParams,
                     pd.yieldPolicy,
                     pd.collections,
                     std::move(rctx.solutions),
                     std::move(rctx.topLevelSampleFieldNames),
                     rctx.hasRelevantMultikeyIndex);
}

StatusWith<PlanRankingResult> CBRPlanRankingStrategy::rankPlans(
    OperationContext* opCtx,
    CanonicalQuery& query,
    QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    QuerySolutionVector solutions,
    StringSet topLevelSampleFieldNames,
    bool hasRelevantMultikeyIndex) const {
    using namespace cost_based_ranker;

    size_t numSolutions = solutions.size();

    const bool isTrivialQuery = isTriviallyEstimable(query);

    if (isTrivialQuery && !hasRelevantMultikeyIndex) {
        // For trivially estimable queries, heuristic CE is sufficient.
        // Note that it does not need top-level field names.
        // We restrict this optimization to plans with no relevant multikey
        // indices, as we cannot estimate the number of keys a multikey index
        // scan would scan without sampling.
        plannerParams.planRankerMode = QueryPlanRankerModeEnum::kHeuristicCE;
    }

    if (plannerParams.planRankerMode == QueryPlanRankerModeEnum::kAutomaticCE ||
        plannerParams.planRankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        auto meTopLevelFields =
            ce::extractTopLevelFieldsFromMatchExpression(query.getPrimaryMatchExpression());
        topLevelSampleFieldNames.merge(meTopLevelFields);
    }

    // Start timer for server status metrics
    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto startTicks = tickSource->getTicks();

    numPlansHistogram.increment(numSolutions);
    numPlansTotal.increment(numSolutions);
    invocationCount.increment();

    std::unique_ptr<ce::SamplingEstimator> samplingEstimator{nullptr};
    std::unique_ptr<ce::ExactCardinalityEstimator> exactCardinality{nullptr};
    if (plannerParams.planRankerMode == QueryPlanRankerModeEnum::kExactCE) {
        exactCardinality = std::make_unique<ce::ExactCardinalityImpl>(
            collections.getMainCollectionAcquisition(), query, opCtx);
    } else if (plannerParams.planRankerMode == QueryPlanRankerModeEnum::kAutomaticCE ||
               plannerParams.planRankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
        samplingEstimator = ce::SamplingEstimatorImpl::makeDefaultSamplingEstimator(
            query,
            CardinalityEstimate{
                CardinalityType{plannerParams.mainCollectionInfo.collStats->getCardinality()},
                EstimationSource::Metadata},
            yieldPolicy,
            collections);

        // Start timer for sampling server status metrics
        auto startSamplingTicks = tickSource->getTicks();

        // If we do not have any fields that we want to sample then we just include all the
        // fields in the sample. This can occur for primary match expressions which are
        // not trivially estimable yet have no top-level fields (eg. $geoNear or $expr).
        samplingEstimator->generateSample(
            topLevelSampleFieldNames.empty()
                ? ce::ProjectionParams{ce::NoProjection{}}
                : ce::TopLevelFieldsProjection{std::move(topLevelSampleFieldNames)});

        auto samplingDurationMicros =
            tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startSamplingTicks);

        samplingMicrosTotal.increment(samplingDurationMicros);
        samplingMicrosHistogram.increment(durationCount<Microseconds>(samplingDurationMicros));

        auto n = samplingEstimator->getSampleSize();
        CurOp::get(opCtx)->debug().getAdditiveMetrics().nDocsSampled = static_cast<uint64_t>(n);
    }

    auto planRankingResult = QueryPlanner::planWithCostBasedRanking(plannerParams,
                                                                    samplingEstimator.get(),
                                                                    exactCardinality.get(),
                                                                    std::move(solutions),
                                                                    query);

    // Calculate duration for server status metrics
    auto durationMicros = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTicks);
    microsHistogram.increment(durationCount<Microseconds>(durationMicros));
    microsTotal.increment(durationMicros);

    if (planRankingResult.isOK() && planRankingResult.getValue().needsWorksMeasuredForPlanCache) {
        CurOp::get(opCtx)->debug().planRankerMethod = PlanRankerMethod::kCostBasedRanker;
    }

    return planRankingResult;
}
}  // namespace plan_ranking
}  // namespace mongo
