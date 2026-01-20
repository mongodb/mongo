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

#include "mongo/db/query/compiler/ce/exact/exact_cardinality.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"

namespace mongo {
namespace plan_ranking {
StatusWith<QueryPlanner::PlanRankingResult> CBRPlanRankingStrategy::rankPlans(
    OperationContext* opCtx,
    CanonicalQuery& query,
    const QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections) const {
    auto rankerMode = plannerParams.planRankerMode;
    // Populating the 'topLevelSampleFields' requires 2 steps:
    //  1. Extract the set of top level fields from the filter, sort and project
    //  components of the CanonicalQuery.
    //  2. Extract the fields of the relevant indexes from the plan() function by passing in
    //  the pointer to 'topLevelSampleFieldNames' as an output parameter.
    auto topLevelSampleFieldNames =
        ce::extractTopLevelFieldsFromMatchExpression(query.getPrimaryMatchExpression());
    auto statusWithMultiPlanSolns =
        QueryPlanner::plan(query, plannerParams, topLevelSampleFieldNames);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus().withContext(
            str::stream() << "error processing query: " << query.toStringForErrorMsg()
                          << " planner returned error");
    }

    using namespace cost_based_ranker;
    std::unique_ptr<ce::SamplingEstimator> samplingEstimator{nullptr};
    std::unique_ptr<ce::ExactCardinalityEstimator> exactCardinality{nullptr};
    if (rankerMode == QueryPlanRankerModeEnum::kExactCE) {
        exactCardinality = std::make_unique<ce::ExactCardinalityImpl>(
            collections.getMainCollectionAcquisition(), query, opCtx);
    } else {
        samplingEstimator = ce::SamplingEstimatorImpl::makeDefaultSamplingEstimator(
            query,
            CardinalityEstimate{
                CardinalityType{plannerParams.mainCollectionInfo.collStats->getCardinality()},
                EstimationSource::Metadata},
            yieldPolicy,
            collections);

        // If we do not have any fields that we want to sample then we just include all the
        // fields in the sample. This can occur if we encounter a find all query with no
        // project or sort specified.
        // TODO: SERVER-108819 We can skip generating the sample entirely in this case and
        // instead use collection cardinality.
        samplingEstimator->generateSample(
            topLevelSampleFieldNames.empty()
                ? ce::ProjectionParams{ce::NoProjection{}}
                : ce::TopLevelFieldsProjection{std::move(topLevelSampleFieldNames)});
    }

    return QueryPlanner::planWithCostBasedRanking(query,
                                                  plannerParams,
                                                  samplingEstimator.get(),
                                                  exactCardinality.get(),
                                                  std::move(statusWithMultiPlanSolns));
}
}  // namespace plan_ranking
}  // namespace mongo
