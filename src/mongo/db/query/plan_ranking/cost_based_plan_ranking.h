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

#pragma once

#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo::plan_ranking {

class CostBasedPlanRankingStrategy {
public:
    ~CostBasedPlanRankingStrategy() = default;

    /**
     * Choose a plan ranking approach - either multi-planning (MP) or CBR for the plans of 'query'
     * based on a cost model of MP and CBR.
     * The main idea is to run multi-planning (MP) for a small number of works sufficient to
     * generate some execution statistics. This statistics is then used to
     * (a) estimate the cost of MP, and
     * (b) extrapolate the remaining number of works needed to fill a batch.
     * Additionally, we estimate the cost of SamplingCE (cost of sample generation + cost of
     * estimating all plans).
     * The two costs are compared wrt the amount of work needed to find the best plan in addition
     * to the work already done by MP. CBR is chosen if it is better than the remaining MP work by
     * some factor (default 2.0 times).
     *
     * Returns the best plan or error.
     */
    StatusWith<QueryPlanner::PlanRankingResult> rankPlans(
        OperationContext* opCtx,
        CanonicalQuery& query,
        QueryPlannerParams& plannerParams,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        const MultipleCollectionAccessor& collections,
        PlannerData plannerData);

    /**
     * Transfer ownership of the working set _ws to the caller.
     */
    std::unique_ptr<WorkingSet> extractWorkingSet();

private:
    std::unique_ptr<WorkingSet> _ws;
};

}  // namespace mongo::plan_ranking

