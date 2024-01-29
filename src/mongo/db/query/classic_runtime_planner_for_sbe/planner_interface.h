/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"

namespace mongo::classic_runtime_planner_for_sbe {

/**
 * Data that any runtime planner needs to perform the planning.
 */
struct PlannerData {
    std::unique_ptr<CanonicalQuery> cq;
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy;
    std::unique_ptr<WorkingSet> workingSet;
    const MultipleCollectionAccessor& collections;
    const QueryPlannerParams& plannerParams;
    boost::optional<size_t> cachedPlanHash;
};

class PlannerInterface {
public:
    virtual ~PlannerInterface() = default;

    /**
     * Function that picks the best plan and returns PlanExecutor for the selected plan. Can be
     * called only once, as it may transfer ownership of some data to returned PlanExecutor.
     */
    virtual std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> plan() = 0;
};

class PlannerBase : public PlannerInterface {
public:
    PlannerBase(OperationContext* opCtx, PlannerData plannerData);

protected:
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> prepareSbePlanExecutor(
        std::unique_ptr<QuerySolution> solution,
        std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> sbePlanAndData,
        bool isFromPlanCache,
        boost::optional<size_t> cachedPlanHash);

    OperationContext* opCtx() {
        return _opCtx;
    }

    CanonicalQuery* cq() {
        return _plannerData.cq.get();
    }

    std::unique_ptr<CanonicalQuery> extractCq() {
        return std::move(_plannerData.cq);
    }

    const MultipleCollectionAccessor& collections() const {
        return _plannerData.collections;
    }

    PlanYieldPolicySBE* sbeYieldPolicy() {
        return _plannerData.sbeYieldPolicy.get();
    }

    std::unique_ptr<PlanYieldPolicySBE> extractSbeYieldPolicy() {
        return std::move(_plannerData.sbeYieldPolicy);
    }

    size_t plannerOptions() const {
        return _plannerData.plannerParams.options;
    }

    boost::optional<size_t> cachedPlanHash() const {
        return _plannerData.cachedPlanHash;
    }

    WorkingSet* ws() {
        return _plannerData.workingSet.get();
    }

    std::unique_ptr<WorkingSet> extractWs() {
        return std::move(_plannerData.workingSet);
    }

private:
    OperationContext* _opCtx;
    PlannerData _plannerData;
};

/**
 * Trivial planner that just creates an executor when there is only one QuerySolution present.
 */
class SingleSolutionPassthroughPlanner final : public PlannerBase {
public:
    SingleSolutionPassthroughPlanner(OperationContext* opCtx,
                                     PlannerData plannerData,
                                     std::unique_ptr<QuerySolution> solution);

    /**
     * Builds and caches SBE plan for the given solution and returns PlanExecutor for it.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> plan() override;

private:
    std::unique_ptr<QuerySolution> _solution;
};

class CachedPlanner final : public PlannerBase {
public:
    CachedPlanner(OperationContext* opCtx,
                  PlannerData plannerData,
                  std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder);

    /**
     * Recovers SBE plan from cache and returns PlanExecutor for it.
     * TODO SERVER-85238 - Implement replanning.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> plan() override;

private:
    std::unique_ptr<sbe::CachedPlanHolder> _cachedPlanHolder;
};

class MultiPlanner final : public PlannerBase {
public:
    MultiPlanner(OperationContext* opCtx,
                 PlannerData plannerData,
                 PlanYieldPolicy::YieldPolicy yieldPolicy,
                 std::vector<std::unique_ptr<QuerySolution>> candidatePlans,
                 PlanCachingMode cachingMode);

    /**
     * Picks the best plan given by the classic engine multiplanner and returns a plan executor. If
     * the planner finished running the best solution during multiplanning, we return the documents
     * and exit, otherwise we pick the best plan and return the SBE plan executor.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> plan() override;

private:
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    std::unique_ptr<MultiPlanStage> _multiPlanStage;
    PlanCachingMode _cachingMode;
};

}  // namespace mongo::classic_runtime_planner_for_sbe
