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

#pragma once

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/sbe_runtime_planner.h"

namespace mongo::sbe {
/**
 * Collects execution stats for all candidate plans, ranks them and picks the best.
 *
 * TODO: add support for backup plan
 */
class MultiPlanner final : public BaseRuntimePlanner {
public:
    MultiPlanner(OperationContext* opCtx,
                 const Collection* collection,
                 const CanonicalQuery& cq,
                 PlanCachingMode cachingMode,
                 PlanYieldPolicySBE* yieldPolicy)
        : BaseRuntimePlanner{opCtx, collection, cq, yieldPolicy}, _cachingMode{cachingMode} {}

    plan_ranker::CandidatePlan plan(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots)
        final;

private:
    /**
     * Returns the best candidate plan selected according to the plan ranking 'decision'.
     *
     * Calls 'close' method on all other candidate plans and updates the plan cache entry,
     * if possible.
     */
    plan_ranker::CandidatePlan finalizeExecutionPlans(
        std::unique_ptr<mongo::plan_ranker::PlanRankingDecision> decision,
        std::vector<plan_ranker::CandidatePlan> candidates) const;

    // Describes the cases in which we should write an entry for the winning plan to the plan cache.
    const PlanCachingMode _cachingMode;
};
}  // namespace mongo::sbe
