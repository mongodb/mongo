// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/write_ops/canonical_delete.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/util/modules.h"

namespace mongo::classic_runtime_planner {

/*
 * Base abstract class for classic runtime planner implementations. Each planner sub-class needs to
 * implement the 'doPlan()' private virtual method.
 */
class ClassicPlannerInterface : public PlannerInterface {
public:
    ClassicPlannerInterface(PlannerData plannerData);

    ClassicPlannerInterface(PlannerData plannerData, PlanExplainerData explainData);

    /**
     * Function which adds the necessary stages for the generated PlanExecutor to perform deletes.
     */
    void addDeleteStage(CanonicalDelete& canonicalDelete,
                        projection_ast::Projection* projection,
                        DeleteStageParams deleteStageParams);
    /**
     * Function which adds the necessary stages for the generated PlanExecutor to perform updates.
     */
    void addUpdateStage(CanonicalUpdate& canonicalUpdate,
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
        std::unique_ptr<CanonicalQuery> canonicalQuery) final;

    /**
     * Extracts the WorkingSet and the root of the executable plan used by this planner.
     */
    SavedExecState extractExecState() &&;

    /**
     * State accessors.
     */
    stage_builder::PlanStageToQsnMap& planStageQsnMap() {
        return _planStageQsnMap;
    }
    virtual const QuerySolution* querySolution() const = 0;
    const CanonicalQuery* cq() const;
    PlanStage* getRoot() const;

protected:
    std::unique_ptr<PlanStage> buildExecutableTree(const QuerySolution& qs);

    void setRoot(std::unique_ptr<PlanStage> root);
    std::unique_ptr<PlanStage> extractRoot() {
        return std::move(_root);
    }

    OperationContext* opCtx();
    const MultipleCollectionAccessor& collections() const;
    PlanYieldPolicy::YieldPolicy yieldPolicy() const;
    const QueryPlannerParams& plannerParams();
    std::shared_ptr<QueryPlannerParams> extractPlannerParams() {
        return std::move(_plannerData.plannerParams);
    }
    size_t plannerOptions() const;
    boost::optional<size_t> cachedPlanHash() const;
    boost::optional<std::string> replanReason() const;
    WorkingSet* ws() const;
    std::unique_ptr<WorkingSet> extractWs() {
        return std::move(_plannerData.workingSet);
    }

    stage_builder::PlanStageToQsnMap _planStageQsnMap;

    /**
     * Planner state enum. Describes the planner status:
     * kNotInitialized: The planner has not made any planning decision yet, so neither a solution
     *       can be extracted nor an executor built.
     * kInitialized: The planner has picked a plan, so either
     *       a solution could be extracted or an executor built.
     * kDisposed: The planner has already either built an executor or
     *       extracted a solution, so no further action can be taken.
     */
    enum { kNotInitialized, kInitialized, kDisposed } _state = kNotInitialized;

private:
    virtual Status doPlan(PlanYieldPolicy* planYieldPolicy) = 0;

    virtual std::unique_ptr<QuerySolution> extractQuerySolution() = 0;

    NamespaceString makeNamespaceString();

    std::unique_ptr<PlanStage> _root;
    NamespaceString _nss;
    PlannerData _plannerData;
    PlanExplainerData _planExplainerData;
};

/**
 * Fast-path planner which creates a classic plan executor for IDHACK queries.
 */
class IdHackPlanner final : public ClassicPlannerInterface {
public:
    IdHackPlanner(PlannerData plannerData, const IndexCatalogEntry* entry);
    PlanRankingResult extractPlanRankingResult() override;

    const QuerySolution* querySolution() const override;

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
                                     std::unique_ptr<QuerySolution> querySolution,
                                     PlanExplainerData explainData);

    SingleSolutionPassthroughPlanner(PlannerData plannerData,
                                     std::unique_ptr<QuerySolution> querySolution,
                                     PlanExplainerData explainData,
                                     ClassicExecState&& state);

    const QuerySolution* querySolution() const override;

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

    const QuerySolution* querySolution() const override;
    PlanRankingResult extractPlanRankingResult() override;

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
    MultiPlanner(PlannerData plannerData,
                 std::vector<std::unique_ptr<QuerySolution>> solutions,
                 PlanExplainerData explainData,
                 bool addingCBRChosenPlanToPlanCache = false);

    /**
     * Runs the trial period by working all candidate plans for as long as given in 'trialConfig'.
     */
    Status runTrials(trial_period::TrialPhaseConfig trialConfig);

    /**
     * Returns the specific stats from the multi-planner stage.
     */
    const MultiPlanStats* getSpecificStats() const;

    /**
     * Picks the best plan among the candidate plans after the trial period has been run.
     */
    Status pickBestPlan();
    std::unique_ptr<QuerySolution> extractQuerySolution() override;
    trial_period::TrialPhaseConfig getTrialPhaseConfig() const {
        return _multiplanStage->getTrialPhaseConfig();
    }

    StatusWith<MultiPlanStage::EstimationResult> estimateAllPlans() const {
        return _multiplanStage->estimateAllPlans();
    }

    /**
     * Extracts all rejected plans along with its stages and the plan stage to QSN map for explain
     * purposes
     */
    PlanExplainerData extractExplainData();

    /**
     * Notifies the multi-planner that CBR has chosen the winning plan. Excludes the
     * finishing-up trial's work/time from the multiplanning stats.
     */
    void markCBRChoseWinner();

    /**
     * Emits the accumulated multi-planner metrics without running a new trial. Used when a capped
     * estimation trial is followed by a non-MP ranker (e.g., CBR) so that metrics are emitted
     * exactly once per planning invocation even when no finishing-up trial runs.
     */
    void emitAccumulatedStats();

    /**
     * Abandons the trial period without picking a best plan thus rejecting all plans
     * except for plans with the given hashes.
     */
    void abandonTrialsExceptHashes(const boost::container::flat_set<size_t>& hashes);

    const QuerySolution* querySolution() const override;

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    MultiPlanStage* _multiplanStage;
};

/**
 * Planner for rooted $or queries. Uses multi-planning for each individual $or clause.
 */
class SubPlanner final : public ClassicPlannerInterface {
public:
    SubPlanner(PlannerData plannerData);

    const QuerySolution* querySolution() const override;

private:
    Status doPlan(PlanYieldPolicy* planYieldPolicy) override;

    std::unique_ptr<QuerySolution> extractQuerySolution() override;

    SubplanStage* _subplanStage;
};
}  // namespace mongo::classic_runtime_planner
