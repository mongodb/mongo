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

#include "mongo/db/query/plan_ranking/plan_ranker.h"

#include "mongo/base/status.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {
namespace plan_ranking {
StatusWith<QueryPlanner::PlanRankingResult> PlanRanker::rankPlans(
    OperationContext* opCtx,
    CanonicalQuery& query,
    const QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    PlannerData plannerData) {
    // TODO SERVER-114514: Implement restrictive approach for automaticCE. Then delete this comment
    // below:
    // For illustration purposes. We'd use multiplanner for ranking like this:
    // auto statusWithMultiPlanSolns = QueryPlanner::plan(query, plannerParams);
    // auto mp =
    //     classic_runtime_planner::MultiPlanner(std::move(plannerData),
    //                                           std::move(statusWithMultiPlanSolns.getValue()),
    //                                           QueryPlanner::PlanRankingResult{});
    // uassertStatusOK(mp.plan()); <- We could also use runTrials or any other method available.
    // Once done, extract the WorkingSet from the MultiPlanner and store it here for further
    // extraction.
    // _ws.emplace(mp.extractWorkingSet());
    auto rankerMode = plannerParams.planRankerMode;
    if (rankerMode != QueryPlanRankerModeEnum::kMultiPlanning) {
        _ws = std::move(plannerData.workingSet);
        return CBRPlanRankingStrategy().rankPlans(
            opCtx, query, plannerParams, yieldPolicy, collections);
    } else {
        /**
         * This is a special plan ranking strategy in that it does not actually rank plans, but
         * rather returns all enumerated plans. This will result in multi-planning being used
         * to select a winning plan at runtime.
         */
        auto statusWithMultiPlanSolns = QueryPlanner::plan(query, plannerParams);
        _ws = std::move(plannerData.workingSet);
        if (!statusWithMultiPlanSolns.isOK()) {
            return statusWithMultiPlanSolns.getStatus().withContext(
                str::stream() << "error processing query: " << query.toStringForErrorMsg()
                              << " planner returned error");
        }
        return QueryPlanner::PlanRankingResult{std::move(statusWithMultiPlanSolns.getValue())};
    }
}

std::unique_ptr<WorkingSet> PlanRanker::extractWorkingSet() {
    tassert(11484500, "WorkingSet is not initialized", _ws);
    auto result = std::move(_ws);
    _ws = nullptr;
    return result;
}
}  // namespace plan_ranking
}  // namespace mongo
