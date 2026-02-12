/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/util/modules.h"

namespace mongo::exec_deferred_engine_choice {

/*
 * Base abstract class for executor runtime planner implementations that defer engine selection
 * until after a query solution is chosen. Provides functionality to lower a query solution to
 * classic or SBE.
 */
class DeferredEngineChoicePlannerInterface : public PlannerInterface {
public:
    DeferredEngineChoicePlannerInterface(PlannerData plannerData);

    std::unique_ptr<PlanStage> buildExecutableTree(const QuerySolution& qs);

    /*
     * Given a query solution and `toSbe` to indicate which engine to lower to, builds and returns
     * a plan executor. When `makeExecutor` is called, subclasses of this can determine which engine
     * to use and call this function to lower depending on the decision.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> executorFromSolution(
        bool toSbe,
        std::unique_ptr<CanonicalQuery> canonicalQuery,
        std::unique_ptr<QuerySolution> querySolution,
        std::unique_ptr<MultiPlanStage> mps,
        Pipeline* pipeline = nullptr);

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeSbePlanExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery,
        std::unique_ptr<QuerySolution> querySolution,
        std::unique_ptr<MultiPlanStage> mps,
        Pipeline* pipeline);

    OperationContext* opCtx() {
        return _plannerData.opCtx;
    }
    CanonicalQuery* cq() {
        return _plannerData.cq;
    }
    const MultipleCollectionAccessor& collections() const {
        return _plannerData.collections;
    }
    PlanYieldPolicy::YieldPolicy yieldPolicy() const {
        return _plannerData.yieldPolicy;
    }
    size_t plannerOptions() const {
        return _plannerData.plannerParams->providedOptions;
    }
    WorkingSet* ws() const {
        return _plannerData.workingSet.get();
    }
    QueryPlannerParams* plannerParams() {
        return _plannerData.plannerParams.get();
    }

protected:
    stage_builder::PlanStageToQsnMap _planStageQsnMap;
    PlannerData _plannerData;
    NamespaceString _nss;
};

/**
 * Trivial planner that just creates a plan executor when there is only one QuerySolution present.
 */
class SingleSolutionPassthroughPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    SingleSolutionPassthroughPlanner(PlannerData plannerData,
                                     std::unique_ptr<QuerySolution> querySolution);

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery, Pipeline* pipeline = nullptr) override;

private:
    std::unique_ptr<QuerySolution> _querySolution;
};

/**
 * Picks the best plan and caches in the classic plan cache, accounting for which engine is being
 * used.
 */
class MultiPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    MultiPlanner(PlannerData plannerData, std::vector<std::unique_ptr<QuerySolution>> solutions);

    /**
     * Returns the specific stats from the multi-planner stage.
     */
    const MultiPlanStats* getSpecificStats() const;

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery, Pipeline* pipeline = nullptr) override;

private:
    void _cacheDependingOnPlan(const CanonicalQuery&,
                               MultiPlanStage& mps,
                               std::unique_ptr<plan_ranker::PlanRankingDecision>,
                               std::vector<plan_ranker::CandidatePlan>&);

    std::unique_ptr<MultiPlanStage> _multiplanStage;
};

/**
 * Picks the best plan for each $or branch and using caching callbacks to cachek either whole plan
 * or each branch in the classic plan cache.
 */
class SubPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    SubPlanner(PlannerData plannerData);

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery, Pipeline* pipeline = nullptr) override;

private:
    std::unique_ptr<SubplanStage> _subPlanStage;
};

}  // namespace mongo::exec_deferred_engine_choice
