// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/plan_ranking_utils.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace cbr = cost_based_ranker;

namespace plan_ranking_tests {

const QuerySolution* pickBestPlan(CanonicalQuery* cq,
                                  OperationContext& opCtx,
                                  boost::intrusive_ptr<ExpressionContext> expCtx,
                                  std::unique_ptr<MultiPlanStage>& mps,
                                  NamespaceString nss) {
    const auto collection = acquireCollection(
        &opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(&opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    MultipleCollectionAccessor collectionsAccessor(collection);
    QueryPlannerParams plannerParams{
        QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = &opCtx,
            .canonicalQuery = *cq,
            .collections = collectionsAccessor,
            .plannerOptions = QueryPlannerParams::DEFAULT,
        },
    };
    // Plan.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, plannerParams);
    ASSERT_OK(statusWithMultiPlanSolns.getStatus());
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());
    ASSERT_GREATER_THAN_OR_EQUALS(solutions.size(), 1U);
    auto temp = plan_cache_util::ClassicPlanCacheWriter{&opCtx, collection};
    mps = std::make_unique<MultiPlanStage>(expCtx.get(), collection, cq, temp);
    std::unique_ptr<WorkingSet> ws(new WorkingSet());
    // Put each solution from the planner into the 'MultiPlanStage'.
    for (size_t i = 0; i < solutions.size(); ++i) {
        auto&& root = stage_builder::buildClassicExecutableTree(
            &opCtx, collection, *cq, *solutions[i], ws.get());
        mps->addPlan(std::move(solutions[i]), std::move(root), ws.get());
    }
    // This is what sets a backup plan, should we test for it.
    NoopYieldPolicy yieldPolicy(&opCtx, opCtx.getServiceContext()->getFastClockSource());
    ASSERT_OK(mps->runTrials(&yieldPolicy));
    ASSERT_OK(mps->pickBestPlan());
    ASSERT(mps->bestPlanChosen());
    auto bestPlanIdx = mps->bestPlanIdx();
    ASSERT(bestPlanIdx.has_value());
    ASSERT_LESS_THAN(*bestPlanIdx, solutions.size());

    // And return a pointer to the best solution.
    return static_cast<const MultiPlanStage*>(mps.get())->bestSolution();
}

const QuerySolution* bestCBRPlan(CanonicalQuery* cq,
                                 size_t numDocs,
                                 OperationContext& opCtx,
                                 int sampleSize,
                                 std::vector<std::unique_ptr<QuerySolution>>& bestCBRPlan,
                                 NamespaceString nss,
                                 SamplingCEMethodEnum samplingStyle,
                                 boost::optional<int> numChunks,
                                 boost::optional<PlanningTimeProfile&> timeProfile) {
    const auto collection = acquireCollection(
        &opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(&opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    MultipleCollectionAccessor collectionsAccessor(collection);

    auto collCard{cbr::CardinalityEstimate{cbr::CardinalityType{static_cast<double>(numDocs)},
                                           cbr::EstimationSource::Metadata}};
    std::unique_ptr<ce::SamplingEstimator> samplingEstimator =
        std::make_unique<ce::SamplingEstimatorImpl>(&opCtx,
                                                    collectionsAccessor,
                                                    nss,
                                                    PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                    static_cast<size_t>(sampleSize),
                                                    samplingStyle,
                                                    numChunks,
                                                    collCard,
                                                    nullptr /*customerQueryExpCtx*/);

    QueryPlannerParams plannerParams{
        QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = &opCtx,
            .canonicalQuery = *cq,
            .collections = collectionsAccessor,
            .plannerOptions = QueryPlannerParams::DEFAULT,
            .cbrEnabled = true,
            .planRankerMode = QueryPlanRankerModeEnum::kSamplingCE,
        },
    };

    // Handle sample generation.
    auto topLevelSampleFieldNames =
        ce::extractTopLevelFieldsFromMatchExpression(cq->getPrimaryMatchExpression());
    auto statusWithMultiPlanSolns =
        QueryPlanner::plan(*cq, plannerParams, topLevelSampleFieldNames);

    Timer generateSampleTimer;
    samplingEstimator->generateSample(
        topLevelSampleFieldNames.empty()
            ? ce::ProjectionParams{ce::NoProjection{}}
            : ce::TopLevelFieldsProjection{std::move(topLevelSampleFieldNames)});
    double generateSampleTimeMS = generateSampleTimer.elapsed().count() / 1000.0;

    Timer planningTimer;
    auto statusWithCBRSolns =
        QueryPlanner::planWithCostBasedRanking(plannerParams,
                                               samplingEstimator.get(),
                                               nullptr,
                                               std::move(statusWithMultiPlanSolns.getValue()),
                                               *cq);
    double planTimeMS = planningTimer.elapsed().count() / 1000.0;

    if (timeProfile.has_value()) {
        timeProfile->sampleGenerationTimeMS = generateSampleTimeMS;
        timeProfile->planRankingTimeMS = planTimeMS;
    }

    ASSERT(statusWithCBRSolns.isOK());
    auto solutions = std::move(statusWithCBRSolns.getValue().solutions);
    ASSERT(solutions.size() == 1);
    const QuerySolution* result = solutions[0].get();
    // The plan itself is owned by bestCBRPlan
    bestCBRPlan.push_back(std::move(solutions[0]));
    return result;
}

}  // namespace plan_ranking_tests
}  // namespace mongo
