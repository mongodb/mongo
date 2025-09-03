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

#include "mongo/db/exec/classic/subplan.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {

using std::unique_ptr;
using std::vector;

const char* SubplanStage::kStageType = "SUBPLAN";

SubplanStage::SubplanStage(ExpressionContext* expCtx,
                           VariantCollectionPtrOrAcquisition collection,
                           WorkingSet* ws,
                           CanonicalQuery* cq,
                           PlanSelectionCallbacks planSelectionCallbacks)
    : RequiresAllIndicesStage(kStageType, expCtx, collection),
      _ws(ws),
      _query(cq),
      _planSelectionCallbacks(std::move(planSelectionCallbacks)) {
    invariant(cq);
    invariant(_query->getPrimaryMatchExpression()->matchType() == MatchExpression::OR);
    invariant(_query->getPrimaryMatchExpression()->numChildren(),
              "Cannot use a SUBPLAN stage for an $or with no children");
}

bool SubplanStage::canUseSubplanning(const CanonicalQuery& query) {
    const FindCommandRequest& findCommand = query.getFindCommandRequest();
    const MatchExpression* expr = query.getPrimaryMatchExpression();

    // Hint provided
    if (!findCommand.getHint().isEmpty()) {
        return false;
    }

    // Min provided
    // Min queries are a special case of hinted queries.
    if (!findCommand.getMin().isEmpty()) {
        return false;
    }

    // Max provided
    // Similar to min, max queries are a special case of hinted queries.
    if (!findCommand.getMax().isEmpty()) {
        return false;
    }

    // Tailable cursors won't get cached, just turn into collscans.
    if (findCommand.getTailable()) {
        return false;
    }

    // Distinct-eligible queries cannot use subplanning.
    if (query.getDistinct()) {
        return false;
    }

    // We can only subplan rooted $or queries, and only if they have at least one clause.
    return MatchExpression::OR == expr->matchType() && expr->numChildren() > 0;
}

Status SubplanStage::choosePlanWholeQuery(const QueryPlannerParams& plannerParams,
                                          PlanYieldPolicy* yieldPolicy,
                                          bool shouldConstructClassicExecutableTree) {
    // Clear out the working set. We'll start with a fresh working set.
    _ws->clear();

    // Use the query planning module to plan the whole query.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(*_query, plannerParams);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus().withContext(
            str::stream() << "error processing query: " << _query->toStringForErrorMsg()
                          << " planner returned error");
    }
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());

    if (1 == solutions.size()) {
        // Only one possible plan.  Run it.  Build the stages from the solution.
        if (shouldConstructClassicExecutableTree) {
            auto&& root = stage_builder::buildClassicExecutableTree(
                expCtx()->getOperationContext(), collection(), *_query, *solutions[0], _ws);
            invariant(_children.empty());
            _children.emplace_back(std::move(root));
        }
        // This SubplanStage takes ownership of the query solution.
        _compositeSolution = std::move(solutions.back());
        solutions.pop_back();

        return Status::OK();
    } else {
        // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
        // and so on. The working set will be shared by all candidate plans.
        invariant(_children.empty());

        _usesMultiplanning = true;

        _children.emplace_back(std::make_unique<MultiPlanStage>(
            expCtx(), collection(), _query, _planSelectionCallbacks.onPickPlanWholeQuery));

        MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(child().get());

        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            solutions[ix]->indexFilterApplied = plannerParams.indexFiltersApplied;

            auto&& nextPlanRoot = stage_builder::buildClassicExecutableTree(
                expCtx()->getOperationContext(), collection(), *_query, *solutions[ix], _ws);
            multiPlanStage->addPlan(std::move(solutions[ix]), std::move(nextPlanRoot), _ws);
        }

        // Delegate the the MultiPlanStage's plan selection facility.
        Status planSelectStat = multiPlanStage->pickBestPlan(yieldPolicy);
        if (!planSelectStat.isOK()) {
            return planSelectStat;
        }

        return Status::OK();
    }
}

Status SubplanStage::pickBestPlan(const QueryPlannerParams& plannerParams,
                                  PlanYieldPolicy* yieldPolicy,
                                  bool shouldConstructClassicExecutableTree) {
    // Adds the amount of time taken by pickBestPlan() to executionTime. There's lots of work that
    // happens here, so this is needed for the time accounting to make sense.
    auto optTimer = getOptTimer();

    // During plan selection, the list of indices we are using to plan must remain stable, so the
    // query will die during yield recovery if any index has been dropped. However, once plan
    // selection completes successfully, we no longer need all indices to stick around. The selected
    // plan should safely die on yield recovery if it is using the dropped index.
    //
    // Dismiss the requirement that no indices can be dropped when this method returns.
    ON_BLOCK_EXIT([this] { releaseAllIndicesRequirement(); });

    std::function<std::unique_ptr<SolutionCacheData>(const CanonicalQuery& cq,
                                                     const CollectionPtr& coll)>
        getSolutionCachedData =
            [](const CanonicalQuery& cq,
               const CollectionPtr& coll) -> std::unique_ptr<SolutionCacheData> {
        auto planCache = CollectionQueryInfo::get(coll).getPlanCache();
        tassert(5969800, "Classic Plan Cache not found", planCache);
        if (shouldCacheQuery(cq)) {
            auto planCacheKey = plan_cache_key_factory::make<PlanCacheKey>(cq, coll);
            if (auto cachedSol = planCache->getCacheEntryIfActive(planCacheKey)) {
                return std::move(cachedSol->cachedPlan);
            }
        }

        return nullptr;
    };

    auto multiCollectionAccessor = [&]() -> MultipleCollectionAccessor {
        if (collection().isAcquisition()) {
            return MultipleCollectionAccessor{collection().getAcquisition()};
        }
        return MultipleCollectionAccessor{collection().getCollectionPtr()};
    }();
    auto rankerMode = _query->getExpCtx()->getQueryKnobConfiguration().getPlanRankerMode();
    // Populating the 'topLevelSampleFieldNames' requires 2 steps:
    //  1. Extract the set of top level fields from the filter, sort and project components of the
    //  CanonicalQuery.
    //  2. Extract the fields of the relevant indexes for each branch of the rooted $or by passing
    //  in the pointer to 'topLevelSampleFieldNames' to planSubqueries().
    StringSet topLevelSampleFieldNames;
    std::unique_ptr<ce::SamplingEstimator> samplingEstimator{nullptr};
    std::unique_ptr<ce::ExactCardinalityEstimator> exactCardinality{nullptr};
    if (rankerMode == QueryPlanRankerModeEnum::kSamplingCE ||
        rankerMode == QueryPlanRankerModeEnum::kAutomaticCE) {
        using namespace cost_based_ranker;
        auto samplingMode =
            _query->getExpCtx()->getQueryKnobConfiguration().getInternalQuerySamplingCEMethod();
        samplingEstimator = std::make_unique<ce::SamplingEstimatorImpl>(
            _query->getOpCtx(),
            multiCollectionAccessor,
            yieldPolicy->getPolicy(),
            samplingMode == SamplingCEMethodEnum::kRandom
                ? ce::SamplingEstimatorImpl::SamplingStyle::kRandom
                : ce::SamplingEstimatorImpl::SamplingStyle::kChunk,
            CardinalityEstimate{
                CardinalityType{plannerParams.mainCollectionInfo.collStats->getCardinality()},
                EstimationSource::Metadata},
            _query->getExpCtx()->getQueryKnobConfiguration().getConfidenceInterval(),
            samplingMarginOfError.load(),
            internalQueryNumChunksForChunkBasedSampling.load());
        topLevelSampleFieldNames =
            ce::extractTopLevelFieldsFromMatchExpression(_query->getPrimaryMatchExpression());
    } else if (rankerMode == QueryPlanRankerModeEnum::kExactCE) {
        exactCardinality = std::make_unique<ce::ExactCardinalityImpl>(
            collectionPtr(), *_query, expCtx()->getOperationContext());
    }

    auto subplanningStatus = samplingEstimator
        ? QueryPlanner::planSubqueries(expCtx()->getOperationContext(),
                                       getSolutionCachedData,
                                       collectionPtr(),
                                       *_query,
                                       plannerParams,
                                       samplingEstimator.get(),
                                       exactCardinality.get(),
                                       topLevelSampleFieldNames)
        : QueryPlanner::planSubqueries(expCtx()->getOperationContext(),
                                       getSolutionCachedData,
                                       collectionPtr(),
                                       *_query,
                                       plannerParams,
                                       samplingEstimator.get(),
                                       exactCardinality.get());


    // Plan each branch of the $or.
    if (rankerMode != QueryPlanRankerModeEnum::kMultiPlanning && subplanningStatus.isOK()) {
        if (rankerMode == QueryPlanRankerModeEnum::kSamplingCE) {
            // If we do not have any fields that we want to sample then we just include all the
            // fields in the sample. This can occur if we encounter a find all query with no project
            // or sort specified.
            // TODO: SERVER-108819 We can skip generating the sample entirely in this case and
            // instead use collection cardinality.
            samplingEstimator->generateSample(
                topLevelSampleFieldNames.empty()
                    ? ce::ProjectionParams{ce::NoProjection{}}
                    : ce::TopLevelFieldsProjection{std::move(topLevelSampleFieldNames)});
        }

        for (const auto& branchResult : subplanningStatus.getValue().branches) {
            auto statusWithCBRSolns =
                QueryPlanner::planWithCostBasedRanking(*branchResult->canonicalQuery,
                                                       plannerParams,
                                                       samplingEstimator.get(),
                                                       exactCardinality.get(),
                                                       std::move(branchResult->solutions));
            if (!statusWithCBRSolns.isOK()) {
                str::stream ss;
                ss << "Can't plan for subchild " << branchResult->canonicalQuery->toString() << " "
                   << statusWithCBRSolns.getStatus().reason();
                subplanningStatus = statusWithCBRSolns.getStatus().withContext(ss);
                break;
            }
            branchResult->solutions = std::move(statusWithCBRSolns.getValue().solutions);
        }
    }

    if (!subplanningStatus.isOK()) {
        return choosePlanWholeQuery(
            plannerParams, yieldPolicy, shouldConstructClassicExecutableTree);
    }

    // Remember whether each branch of the $or was planned from a cached solution.
    auto subplanningResult = std::move(subplanningStatus.getValue());
    _branchPlannedFromCache.clear();
    for (auto&& branch : subplanningResult.branches) {
        _branchPlannedFromCache.push_back(branch->cachedData != nullptr);
    }

    // Use the multi plan stage to select a winning plan for each branch, and then construct
    // the overall winning plan from the resulting index tags.
    auto multiplanCallback = [&](CanonicalQuery* cq,
                                 std::vector<std::unique_ptr<QuerySolution>> solutions)
        -> StatusWith<std::unique_ptr<QuerySolution>> {
        _ws->clear();

        // We temporarily add the MPS to _children to ensure that we pass down all save/restore
        // messages that can be generated if pickBestPlan yields.
        invariant(_children.empty());
        _children.emplace_back(std::make_unique<MultiPlanStage>(
            expCtx(),
            collection(),
            cq,
            // Copy the callback function object since we have to use it for multiple branches.
            _planSelectionCallbacks.onPickPlanForBranch));
        ON_BLOCK_EXIT([&] {
            invariant(_children.size() == 1);  // Make sure nothing else was added to _children.
            _children.pop_back();
        });
        MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(child().get());

        // Dump all the solutions into the MPS.
        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            auto&& nextPlanRoot = stage_builder::buildClassicExecutableTree(
                expCtx()->getOperationContext(), collection(), *cq, *solutions[ix], _ws);

            multiPlanStage->addPlan(std::move(solutions[ix]), std::move(nextPlanRoot), _ws);
        }

        Status planSelectStat = multiPlanStage->pickBestPlan(yieldPolicy);
        if (!planSelectStat.isOK()) {
            return planSelectStat;
        }

        if (!multiPlanStage->bestPlanChosen()) {
            str::stream ss;
            ss << "Failed to pick best plan for subchild " << cq->toStringForErrorMsg();
            return Status(ErrorCodes::NoQueryExecutionPlans, ss);
        }
        return multiPlanStage->extractBestSolution();
    };
    auto subplanSelectStat = QueryPlanner::choosePlanForSubqueries(
        *_query, plannerParams, std::move(subplanningResult), multiplanCallback);
    if (!subplanSelectStat.isOK()) {
        if (subplanSelectStat != ErrorCodes::NoQueryExecutionPlans) {
            // Query planning can continue if we failed to find a solution for one of the
            // children. Otherwise, it cannot, as it may no longer be safe to access the collection
            // (and index may have been dropped, we may have exceeded the time limit, etc).
            return subplanSelectStat.getStatus();
        }
        return choosePlanWholeQuery(
            plannerParams, yieldPolicy, shouldConstructClassicExecutableTree);
    }

    // Build a plan stage tree from the the composite solution and add it as our child stage.
    _compositeSolution = std::move(subplanSelectStat.getValue());

    if (shouldConstructClassicExecutableTree) {
        invariant(_children.empty());
        auto&& root = stage_builder::buildClassicExecutableTree(
            expCtx()->getOperationContext(), collection(), *_query, *_compositeSolution, _ws);
        _children.emplace_back(std::move(root));
    }

    _ws->clear();

    return Status::OK();
}

bool SubplanStage::isEOF() const {
    // If we're running we best have a runner.
    invariant(child());
    return child()->isEOF();
}

PlanStage::StageState SubplanStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    invariant(child());
    return child()->work(out);
}

unique_ptr<PlanStageStats> SubplanStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_SUBPLAN);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SubplanStage::getSpecificStats() const {
    return nullptr;
}
}  // namespace mongo
