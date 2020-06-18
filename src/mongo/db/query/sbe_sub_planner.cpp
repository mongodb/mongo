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
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/stage_builder_util.h"

namespace mongo::sbe {
plan_ranker::CandidatePlan SubPlanner::plan(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    // Plan each branch of the $or.
    auto subplanningStatus =
        QueryPlanner::planSubqueries(_opCtx,
                                     _collection,
                                     CollectionQueryInfo::get(_collection).getPlanCache(),
                                     _cq,
                                     _queryParams);
    if (!subplanningStatus.isOK()) {
        return planWholeQuery();
    }

    auto multiplanCallback = [&](CanonicalQuery* cq,
                                 std::vector<std::unique_ptr<QuerySolution>> solutions)
        -> StatusWith<std::unique_ptr<QuerySolution>> {
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots;
        for (auto&& solution : solutions) {
            roots.push_back(stage_builder::buildSlotBasedExecutableTree(
                _opCtx, _collection, *cq, *solution, _yieldPolicy, true));
        }

        // We pass the SometimesCache option to the MPS because the SubplanStage currently does
        // not use the 'CachedSolutionPlanner' eviction mechanism. We therefore are more
        // conservative about putting a potentially bad plan into the cache in the subplan path.
        MultiPlanner multiPlanner{
            _opCtx, _collection, *cq, PlanCachingMode::SometimesCache, _yieldPolicy};
        auto plan = multiPlanner.plan(std::move(solutions), std::move(roots));
        return std::move(plan.solution);
    };

    auto subplanSelectStat = QueryPlanner::choosePlanForSubqueries(
        _cq, _queryParams, std::move(subplanningStatus.getValue()), multiplanCallback);
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
    auto&& [root, data] = stage_builder::buildSlotBasedExecutableTree(
        _opCtx, _collection, _cq, *compositeSolution, _yieldPolicy, false);
    prepareExecutionPlan(root.get(), &data);
    return {std::move(compositeSolution), std::move(root), std::move(data)};
}

plan_ranker::CandidatePlan SubPlanner::planWholeQuery() const {
    // Use the query planning module to plan the whole query.
    auto solutions = uassertStatusOK(QueryPlanner::plan(_cq, _queryParams));

    // Only one possible plan. Build the stages from the solution.
    if (solutions.size() == 1) {
        auto&& [root, data] = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collection, _cq, *solutions[0], _yieldPolicy, false);
        prepareExecutionPlan(root.get(), &data);
        return {std::move(solutions[0]), std::move(root), std::move(data)};
    }

    // Many solutions. Build a plan stage tree for each solution and create a multi planner to pick
    // the best, update the cache, and so on.
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots;
    for (auto&& solution : solutions) {
        roots.push_back(stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collection, _cq, *solution, _yieldPolicy, true));
    }

    MultiPlanner multiPlanner{_opCtx, _collection, _cq, PlanCachingMode::AlwaysCache, _yieldPolicy};
    return multiPlanner.plan(std::move(solutions), std::move(roots));
}
}  // namespace mongo::sbe
