/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"

#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/stats/counters.h"

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

StatusWith<PlanRankingResult> CBRPlanRankingStrategy::rankPlans(PlannerData& pd) {
    return rankPlans(pd.opCtx, *pd.cq, *pd.plannerParams, pd.yieldPolicy, pd.collections);
}

StatusWith<PlanRankingResult> CBRPlanRankingStrategy::rankPlans(
    OperationContext* opCtx,
    CanonicalQuery& query,
    QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections) const {
    using namespace cost_based_ranker;

    const bool isTrivialQuery = isTriviallyEstimable(query);

    StringSet topLevelSampleFieldNames;
    bool hasRelevantMultikeyIndex = false;

    // Populating the 'topLevelSampleFields' requires 2 steps:
    //  1. Extract the fields of the relevant indexes from the plan() function by passing in
    //  the pointer to 'topLevelSampleFieldNames' as an output parameter.
    //  We do this also for trivially estimable queries, as the presence of relevant multikey
    //  indices prevent us from switching to heuristicCE.
    //  2. Extract the set of top level fields from the filter, sort and project
    //  components of the CanonicalQuery. We do this only for CE methods which might use sampling.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(
        query,
        plannerParams,
        topLevelSampleFieldNames,
        isTrivialQuery ? boost::optional<bool&>(hasRelevantMultikeyIndex) : boost::none);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus().withContext(
            str::stream() << "error processing query: " << query.toStringForErrorMsg()
                          << " planner returned error");
    }

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

    size_t numSolutions = statusWithMultiPlanSolns.getValue().size();

    // Start timer for server status metrics
    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto startTicks = tickSource->getTicks();

    numPlansHistogram.increment(numSolutions);
    numPlansTotal.increment(numSolutions);
    invocationCount.increment();

    if (numSolutions == 1 &&
        (!query.getExplain() ||
         // TODO(SERVER-118659): Remove this disjunction once we support costing count_scan
         QueryPlannerAnalysis::isCountScan(statusWithMultiPlanSolns.getValue()[0].get()))) {
        // TODO SERVER-115496. Make sure this short circuit logic is also taken to main plan_ranking
        // so it applies everywhere. Only one solution, no need to rank.
        std::vector<std::unique_ptr<QuerySolution>> solns;
        solns.push_back(std::move(statusWithMultiPlanSolns.getValue()[0]));
        return PlanRankingResult{.solutions = std::move(solns)};
    }

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

    auto planRankingResult =
        QueryPlanner::planWithCostBasedRanking(plannerParams,
                                               samplingEstimator.get(),
                                               exactCardinality.get(),
                                               std::move(statusWithMultiPlanSolns),
                                               query.getExplain().has_value());

    // Calculate duration for server status metrics
    auto durationMicros = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTicks);
    microsHistogram.increment(durationCount<Microseconds>(durationMicros));
    microsTotal.increment(durationMicros);

    return planRankingResult;
}
}  // namespace plan_ranking
}  // namespace mongo
