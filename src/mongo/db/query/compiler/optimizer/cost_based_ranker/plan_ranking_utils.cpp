/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/multi_plan.h"
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

#include <boost/move/utility_core.hpp>
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
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
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
    auto temp = plan_cache_util::ClassicPlanCacheWriter{
        &opCtx, collection, false /*executeInSbe*/
    };
    mps = std::make_unique<MultiPlanStage>(expCtx.get(), collection, cq, temp);
    std::unique_ptr<WorkingSet> ws(new WorkingSet());
    // Put each solution from the planner into the 'MultiPlanStage'.
    for (size_t i = 0; i < solutions.size(); ++i) {
        auto&& root = stage_builder::buildClassicExecutableTree(
            &opCtx, collection, *cq, *solutions[i], ws.get());
        mps->addPlan(std::move(solutions[i]), std::move(root), ws.get());
    }
    // This is what sets a backup plan, should we test for it.
    NoopYieldPolicy yieldPolicy(&opCtx,
                                opCtx.getServiceContext()->getFastClockSource(),
                                PlanYieldPolicy::YieldThroughAcquisitions{});
    mps->pickBestPlan(&yieldPolicy).transitional_ignore();
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
                                 ce::SamplingEstimatorImpl::SamplingStyle samplingStyle,
                                 boost::optional<int> numChunks) {
    const auto collection = acquireCollection(
        &opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(&opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    MultipleCollectionAccessor collectionsAccessor(collection);

    auto collCard{cbr::CardinalityEstimate{cbr::CardinalityType{static_cast<double>(numDocs)},
                                           cbr::EstimationSource::Metadata}};
    std::unique_ptr<ce::SamplingEstimator> samplingEstimator =
        std::make_unique<ce::SamplingEstimatorImpl>(&opCtx,
                                                    collectionsAccessor,
                                                    PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                    static_cast<size_t>(sampleSize),
                                                    samplingStyle,
                                                    numChunks,
                                                    collCard);

    QueryPlannerParams plannerParams{
        QueryPlannerParams::ArgsForSingleCollectionQuery{
            .opCtx = &opCtx,
            .canonicalQuery = *cq,
            .collections = collectionsAccessor,
            .plannerOptions = QueryPlannerParams::DEFAULT,
            .planRankerMode = QueryPlanRankerModeEnum::kSamplingCE,
        },
    };

    // Handle sample generation.
    auto topLevelSampleFieldNames =
        ce::extractTopLevelFieldsFromMatchExpression(cq->getPrimaryMatchExpression());
    auto statusWithMultiPlanSolns =
        QueryPlanner::plan(*cq, plannerParams, topLevelSampleFieldNames);
    samplingEstimator->generateSample(
        topLevelSampleFieldNames.empty()
            ? ce::ProjectionParams{ce::NoProjection{}}
            : ce::TopLevelFieldsProjection{std::move(topLevelSampleFieldNames)});

    auto statusWithCBRSolns = QueryPlanner::planWithCostBasedRanking(
        *cq, plannerParams, samplingEstimator.get(), nullptr, std::move(statusWithMultiPlanSolns));
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
