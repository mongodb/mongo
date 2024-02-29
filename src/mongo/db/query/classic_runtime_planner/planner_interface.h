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

#include "mongo/db/exec/batched_delete_stage.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/planner_interface.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo::classic_runtime_planner {

/*
 * Base abstract class for classic runtime planner implementations. Each planner sub-class needs to
 * implement the 'doPlan()' private virtual method.
 */
class ClassicPlannerInterface : public PlannerInterface {
public:
    ClassicPlannerInterface(PlannerData plannerData);

    /**
     * Function which adds the necessary stages for the generated PlanExecutor to perform deletes.
     */
    void addDeleteStage(ParsedDelete* parsedDelete,
                        projection_ast::Projection* projection,
                        std::unique_ptr<DeleteStageParams> deleteStageParams);
    /**
     * Function which adds the necessary stages for the generated PlanExecutor to perform updates.
     */
    void addUpdateStage(ParsedUpdate* parsedUpdate,
                        projection_ast::Projection* projection,
                        UpdateStageParams updateStageParams);
    /**
     * Function which adds the necessary stages for the generated PlanExecutor to perform counts.
     */
    void addCountStage(long long limit, long long skip);

    /**
     * Function that picks the best plan if needed. Returns the status if the planning process
     * failed. Must always be called before 'makeExecutor()'.
     */
    Status plan();

    /**
     * Function that creates a PlanExecutor for the selected plan. Can be called only once, as it
     * may transfer ownership of some data to returned PlanExecutor.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override final;

protected:
    std::unique_ptr<PlanStage> buildExecutableTree(const QuerySolution& qs);

    void setRoot(std::unique_ptr<PlanStage> root);

    OperationContext* opCtx();
    CanonicalQuery* cq();
    QuerySolution* querySolution();
    const MultipleCollectionAccessor& collections() const;
    PlanYieldPolicy::YieldPolicy yieldPolicy() const;
    const QueryPlannerParams& plannerParams();
    size_t plannerOptions() const;
    boost::optional<size_t> cachedPlanHash() const;
    WorkingSet* ws() const;

private:
    virtual Status doPlan(PlanYieldPolicy* planYieldPolicy) = 0;

    virtual std::unique_ptr<QuerySolution> extractQuerySolution() = 0;

    NamespaceString makeNamespaceString();

    enum { kNotInitialized, kInitialized, kDisposed } _state = kNotInitialized;
    std::unique_ptr<PlanStage> _root;
    NamespaceString _nss;
    PlannerData _plannerData;
};

/**
 * Fast-path planner which creates a classic plan executor for IDHACK queries.
 */
class IdHackPlanner final : public ClassicPlannerInterface {
public:
    IdHackPlanner(PlannerData plannerData, const IndexDescriptor* descriptor);

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    std::unique_ptr<QuerySolution> extractQuerySolution() override;
};

/**
 * Trivial planner that just creates a classic plan executor when there is only one QuerySolution
 * present.
 */
class SingleSolutionPassthroughPlanner final : public ClassicPlannerInterface {
public:
    SingleSolutionPassthroughPlanner(PlannerData plannerData,
                                     std::unique_ptr<QuerySolution> querySolution);

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    std::unique_ptr<QuerySolution> extractQuerySolution() override;

    std::unique_ptr<QuerySolution> _querySolution;
};

/**
 * Planner which uses a cached solution to generate a classic plan executor. Resorts to
 * multi-planning if re-planning is required.
 */
class CachedPlanner final : public ClassicPlannerInterface {
public:
    CachedPlanner(PlannerData plannerData,
                  std::unique_ptr<CachedSolution> cachedSolution,
                  std::unique_ptr<QuerySolution> querySolution);

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    std::unique_ptr<QuerySolution> extractQuerySolution() override;

    CachedPlanStage* _cachedPlanStage;
    std::unique_ptr<QuerySolution> _querySolution;
};

/**
 * Picks the best plan and returns a classic plan executor.
 */
class MultiPlanner final : public ClassicPlannerInterface {
public:
    MultiPlanner(PlannerData plannerData, std::vector<std::unique_ptr<QuerySolution>> solutions);

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    std::unique_ptr<QuerySolution> extractQuerySolution() override;

    MultiPlanStage* _multiplanStage;
};

/**
 * Planner for rooted $or queries. Uses multi-planning for each individual $or clause.
 */
class SubPlanner final : public ClassicPlannerInterface {
public:
    SubPlanner(PlannerData plannerData);

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    std::unique_ptr<QuerySolution> extractQuerySolution() override;

    SubplanStage* _subplanStage;
};
}  // namespace mongo::classic_runtime_planner
