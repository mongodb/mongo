/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_sub_planner.h"

#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo::sbe {
CandidatePlans SubPlanner::plan(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>>) {

    const auto& mainColl = _collections.getMainCollection();
    // Plan each branch of the $or.
    auto subplanningStatus = QueryPlanner::planSubqueries(
        _opCtx, {} /* getSolutionCachedData */, mainColl, _cq, _queryParams);
    if (!subplanningStatus.isOK()) {
        return planWholeQuery();
    }

    auto multiplanCallback = [&](CanonicalQuery* cq,
                                 std::vector<std::unique_ptr<QuerySolution>> solutions)
        -> StatusWith<std::unique_ptr<QuerySolution>> {
        // One of the indexes in '_queryParams' might have been dropped while planning a previous
        // branch of the OR query. In this case, fail with a 'QueryPlanKilled' error.
        _indexExistenceChecker.check();

        // Ensure that no previous plans are registered to yield while we multi plan each branch.
        _yieldPolicy->clearRegisteredPlans();

        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots;
        for (auto&& solution : solutions) {
            roots.push_back(stage_builder::buildSlotBasedExecutableTree(
                _opCtx, _collections, *cq, *solution, _yieldPolicy));
        }

        // Clear any plans registered to yield once multiplanning is done for this branch. We don't
        // want to leave dangling pointers to the execution plans used in multi planning hanging
        // around in the YieldPolicy.
        ON_BLOCK_EXIT([this]() { _yieldPolicy->clearRegisteredPlans(); });

        // We pass the SometimesCache option to the MPS because the SubplanStage currently does
        // not use the 'CachedSolutionPlanner' eviction mechanism. We therefore are more
        // conservative about putting a potentially bad plan into the cache in the subplan path.
        MultiPlanner multiPlanner{
            _opCtx, _collections, *cq, _queryParams, PlanCachingMode::SometimesCache, _yieldPolicy};
        auto&& [candidates, winnerIdx] = multiPlanner.plan(std::move(solutions), std::move(roots));
        invariant(winnerIdx < candidates.size());
        return std::move(candidates[winnerIdx].solution);
    };

    auto subplanSelectStat = QueryPlanner::choosePlanForSubqueries(
        _cq, _queryParams, std::move(subplanningStatus.getValue()), multiplanCallback);

    // One of the indexes in '_queryParams' might have been dropped while planning the final branch
    // of the OR query. In this case, fail with a 'QueryPlanKilled' error.
    _indexExistenceChecker.check();

    if (!subplanSelectStat.isOK()) {
        // Query planning can continue if we failed to find a solution for one of the children.
        // Otherwise, it cannot, as it may no longer be safe to access the collection (an index
        // may have been dropped, we may have exceeded the time limit, etc).
        uassert(4822874,
                subplanSelectStat.getStatus().reason(),
                subplanSelectStat == ErrorCodes::NoQueryExecutionPlans);
        return planWholeQuery();
    }

    // Build a plan stage tree from a composite solution.
    auto compositeSolution = std::move(subplanSelectStat.getValue());

    // If some agg pipeline stages are being pushed down, extend the solution with them.
    if (!_cq.pipeline().empty()) {
        compositeSolution = QueryPlanner::extendWithAggPipeline(
            _cq, std::move(compositeSolution), _queryParams.secondaryCollectionsInfo);
    }

    auto&& [root, data] = stage_builder::buildSlotBasedExecutableTree(
        _opCtx, _collections, _cq, *compositeSolution, _yieldPolicy);
    auto status = prepareExecutionPlan(root.get(), &data);
    uassertStatusOK(status);
    auto [result, recordId, exitedEarly] = status.getValue();
    tassert(5323804, "sub-planner unexpectedly exited early during prepare phase", !exitedEarly);

    plan_cache_util::updatePlanCache(_opCtx, _collections, _cq, *compositeSolution, *root, data);

    return {makeVector(plan_ranker::CandidatePlan{
                std::move(compositeSolution), std::move(root), std::move(data)}),
            0};
}

CandidatePlans SubPlanner::planWholeQuery() const {
    // Use the query planning module to plan the whole query.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(_cq, _queryParams);
    auto solutions = uassertStatusOK(std::move(statusWithMultiPlanSolns));

    // Only one possible plan. Build the stages from the solution.
    if (solutions.size() == 1) {
        // If some agg pipeline stages are being pushed down, extend the solution with them.
        if (!_cq.pipeline().empty()) {
            solutions[0] = QueryPlanner::extendWithAggPipeline(
                _cq, std::move(solutions[0]), _queryParams.secondaryCollectionsInfo);
        }

        auto&& [root, data] = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collections, _cq, *solutions[0], _yieldPolicy);
        auto status = prepareExecutionPlan(root.get(), &data);
        uassertStatusOK(status);
        auto [result, recordId, exitedEarly] = status.getValue();
        tassert(
            5323805, "sub-planner unexpectedly exited early during prepare phase", !exitedEarly);
        return {makeVector(plan_ranker::CandidatePlan{
                    std::move(solutions[0]), std::move(root), std::move(data)}),
                0};
    }

    // Many solutions. Build a plan stage tree for each solution and create a multi planner to pick
    // the best, update the cache, and so on.
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots;
    for (auto&& solution : solutions) {
        roots.push_back(stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collections, _cq, *solution, _yieldPolicy));
    }

    MultiPlanner multiPlanner{
        _opCtx, _collections, _cq, _queryParams, PlanCachingMode::AlwaysCache, _yieldPolicy};
    return multiPlanner.plan(std::move(solutions), std::move(roots));
}
}  // namespace mongo::sbe
