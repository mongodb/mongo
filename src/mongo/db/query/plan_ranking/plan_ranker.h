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

#pragma once

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"


namespace mongo {
namespace plan_ranking {

struct PlanRankingResult {
    std::vector<std::unique_ptr<QuerySolution>> solutions;
    boost::optional<PlanExplainerData> maybeExplainData;
    // True if these plans were chosen without a pre-execution trial run that measured the
    // 'work' metric (for example, selected by a non-multiplanner). Such plans must be
    // run in a pre-execution phase to measure the amount of work done to produce the
    // first batch, so they can be considered for insertion into the classic plan cache.
    bool needsWorksMeasured{false};

    // Ranker strategies may involve execution; they can return execution-relevant state
    // here, and the caller can choose to resume execution from that point.
    // (e.g., MultiPlanStage may contain spooled results, partially evaluated ixscans, etc.)
    // If none, the caller should consume the provided solution(s) as-is.
    boost::optional<mongo::classic_runtime_planner::SavedExecState> execState;
};

class PlanRankingStrategy {
public:
    virtual StatusWith<plan_ranking::PlanRankingResult> rankPlans(PlannerData& pd) = 0;

    virtual ~PlanRankingStrategy() = default;
};

std::unique_ptr<PlanRankingStrategy> makeStrategy(
    QueryPlanRankerModeEnum rankerMode,
    QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum autoStrategy);

/**
 * Maximum number of plans of $or queries for which the automatic ranking strategy uses whole query
 * planning. For a larger number of plans switch to the subplanner to plan $or branches
 * individually.
 */
static constexpr size_t kMaxNumberOfOrPlans = 16;

/**
 * Check if the optimizer should delay calling of the Subplanner for rooted $or queries until we
 * know the number of plans. The subplanner can be completely skipped if the number of plans is
 * smaller than the kMaxNumberOrPlans.
 */
bool delayOrSkipSubplanner(const CanonicalQuery& query,
                           const QueryPlannerParams& params,
                           bool isClassicEngine);

/**
 * The PlanRanker is responsible for ranking candidate query plans and selecting the best plan(s)
 * to be executed.
 *
 * It will work as a dispatcher to the appropriate plan ranking strategy based on the provided plan
 * ranking mode. Currently, it supports both cost-based ranking (CBR) and multi-planning strategies.
 */
class PlanRanker {
public:
    // If the plan will be executed in SBE (i.e. when 'isClassic' is false) then we will not use one
    // of the CBR fallback strategies for plan ranking and instead use multiplanning.
    // TODO SERVER-117707: Remove this restriction.
    StatusWith<PlanRankingResult> rankPlans(
        OperationContext* opCtx,
        CanonicalQuery& query,
        QueryPlannerParams& plannerParams,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        const MultipleCollectionAccessor& collections,
        // PlannerData for classic multiplanner. We only need the classic one since
        // multiplanning only runs with classic, even if SBE is enabled.
        PlannerData multiPlannerData,
        bool isClassic);
};
}  // namespace plan_ranking
}  // namespace mongo
