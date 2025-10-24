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

#include "mongo/db/query/compiler/optimizer/join/executor.h"

#include "mongo/base/status_with.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::join_ordering {
namespace {
bool anySecondaryNamespacesDontExist(const MultipleCollectionAccessor& mca) {
    auto colls = mca.getSecondaryCollectionAcquisitions();
    return std::any_of(
        colls.begin(), colls.end(), [](auto&& it) { return !it.second.collectionExists(); });
}

bool isAggEligibleForJoinReordering(const MultipleCollectionAccessor& mca,
                                    const Pipeline& pipeline) {
    if (!pipeline.getContext()->getQueryKnobConfiguration().isJoinOrderingEnabled()) {
        return false;
    }

    if (!mca.hasMainCollection()) {
        // We can't determine if the base collection is sharded.
        return false;
    }

    if (mca.getMainCollectionAcquisition().getShardingDescription().isSharded()) {
        // We don't permit a sharded base collection.
        return false;
    }

    if (mca.isAnySecondaryNamespaceAViewOrNotFullyLocal() || anySecondaryNamespacesDontExist(mca)) {
        // TODO SERVER-112239: Enable support for views, as the above check will prevent views from
        // being used for join ordering.
        return false;
    }

    return AggJoinModel::pipelineEligibleForJoinReordering(pipeline);
}
}  // namespace

/**
 * Attempts to apply join optimization to the given aggregation, but if it fails to extract a join
 * model, falls back to preparing executors for the pipeline in the normal way.
 */
StatusWith<JoinReorderedExecutorResult> getJoinReorderedExecutor(
    const MultipleCollectionAccessor& mca,
    const Pipeline& pipeline,
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext> expCtx) {
    // Quick eligibility check.
    if (!isAggEligibleForJoinReordering(mca, pipeline)) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Pipeline or collection ineligible for join-reordering");
    }

    // Try to build JoinGraph.
    auto swModel = AggJoinModel::constructJoinModel(pipeline);
    if (!swModel.isOK()) {
        // We failed to apply join-reordering, so we take the regular path.
        const auto status = swModel.getStatus();
        LOGV2_DEBUG(11083903, 5, "Unable to construct join model", "status"_attr = status);
        return status;
    }

    LOGV2_DEBUG(11083902,
                5,
                "Join model was successfully constructed, reordering joins",
                "graph"_attr = swModel.getValue().toString(/*pretty*/ true));
    auto model = std::move(swModel.getValue());

    // Select access plans for each table in the join.
    auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;
    optimizer::SamplingEstimatorMap samplingEstimators =
        optimizer::makeSamplingEstimators(mca, model.graph, yieldPolicy);
    auto swAccessPlans =
        optimizer::singleTableAccessPlans(opCtx, mca, model.graph, samplingEstimators);
    if (!swAccessPlans.isOK()) {
        return swAccessPlans.getStatus();
    }

    // Construct random-order join graph.
    auto& accessPlans = swAccessPlans.getValue();
    auto qsn = constructSolutionWithRandomOrder(
        std::move(accessPlans.solns),
        model.graph,
        model.resolvedPaths,
        mca,
        expCtx->getQueryKnobConfiguration().getRandomJoinOrderSeed());

    // Lower to SBE.
    // TODO SERVER-111581: permit the use of a different base collection for this query.
    // TODO SERVER-112232: Identify SBE suffixes that are eligible for pushdown & push them to the
    // SBE executor.
    auto& baseCQ = *model.graph.getNode(0).accessPath;
    auto baseNss = baseCQ.nss();
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(opCtx, yieldPolicy, mca, baseNss);
    auto planStagesAndData =
        stage_builder::buildSlotBasedExecutableTree(opCtx, mca, baseCQ, *qsn, sbeYieldPolicy.get());
    stage_builder::prepareSlotBasedExecutableTree(opCtx,
                                                  planStagesAndData.first.get(),
                                                  &planStagesAndData.second,
                                                  baseCQ,
                                                  mca,
                                                  sbeYieldPolicy.get(),
                                                  false /*preparingFromCache*/,
                                                  nullptr /*remoteCursors*/);
    LOGV2_DEBUG(11083905,
                5,
                "SBE plan for join-reordered query",
                "sbePlan"_attr = sbe::DebugPrinter{}.print(planStagesAndData.first->debugPrint()));

    // If there is a pipeline suffix, then that suffix will execute inside a PlanExecutorPipeline,
    // which expects to received owned BSON objects from the inner PlanExecutor.
    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (model.suffix && model.suffix->peekFront()) {
        plannerOptions |= QueryPlannerParams::RETURN_OWNED_DATA;
    }

    // We actually have several canonical queries, so we don't try to pass one in.
    auto exec =
        uassertStatusOKWithLocation(plan_executor_factory::make(opCtx,
                                                                nullptr /* cq */,
                                                                std::move(qsn),
                                                                std::move(planStagesAndData),
                                                                mca,
                                                                plannerOptions,
                                                                mca.getMainCollection()->ns(),
                                                                std::move(sbeYieldPolicy),
                                                                false /* isFromPlanCache */,
                                                                false /* cachedPlanHash */));

    return JoinReorderedExecutorResult{.executor = std::move(exec), .model = std::move(model)};
}
}  // namespace mongo::join_ordering
