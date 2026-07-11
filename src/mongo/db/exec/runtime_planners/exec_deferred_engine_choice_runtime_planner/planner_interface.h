// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/util/modules.h"

namespace mongo::exec_deferred_engine_choice {

std::vector<std::unique_ptr<QuerySolution>> makeQsnResult(std::unique_ptr<QuerySolution> qsn);

/*
 * Base abstract class for executor runtime planner implementations that defer engine selection
 * until after a query solution is chosen. Provides functionality to lower a query solution to
 * classic or SBE.
 */
class DeferredEngineChoicePlannerInterface : public PlannerInterface {
public:
    DeferredEngineChoicePlannerInterface(PlannerData plannerData);

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
    boost::optional<size_t> cachedPlanHash() {
        return _plannerData.cachedPlanHash;
    }
    boost::optional<std::string> replanReason() {
        if (!_plannerData.plannerParams->replanningData) {
            return boost::none;
        }
        return _plannerData.plannerParams->replanningData->replanReason;
    }

protected:
    std::unique_ptr<WorkingSet> extractWs() {
        return std::move(_plannerData.workingSet);
    }
    std::shared_ptr<QueryPlannerParams> extractPlannerParams() {
        return std::move(_plannerData.plannerParams);
    }

    std::unique_ptr<PlanStage> buildExecutableTree(const QuerySolution& qs);

    stage_builder::PlanStageToQsnMap _planStageQsnMap;
    PlannerData _plannerData;
};

/**
 * Trivial planner that just creates a plan executor when there is only one QuerySolution present.
 */
class SingleSolutionPassthroughPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    SingleSolutionPassthroughPlanner(PlannerData plannerData,
                                     std::unique_ptr<QuerySolution> querySolution,
                                     boost::optional<PlanExplainerData> maybeExplainData = {});

    PlanRankingResult extractPlanRankingResult() override;

private:
    std::unique_ptr<QuerySolution> _querySolution;
    boost::optional<PlanExplainerData> _maybeExplainData;
};

/**
 * A planner that holds already known PlanRankingResult. Used when we get PlanRankingResult from
 * CBR.
 */
class PreComputedRankingResultPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    PreComputedRankingResultPlanner(PlannerData plannerData, PlanRankingResult result);

    PlanRankingResult extractPlanRankingResult() override;

private:
    PlanRankingResult _result;
};

/**
 * Picks the best plan and caches in the classic plan cache, accounting for which engine is being
 * used.
 */
class MultiPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    MultiPlanner(PlannerData plannerData,
                 std::vector<std::unique_ptr<QuerySolution>> solutions,
                 bool addingCBRChosenPlanToPlanCache = false,
                 boost::optional<PlanExplainerData> maybeExplainData = boost::none);

    /**
     * Returns the specific stats from the multi-planner stage.
     */
    const MultiPlanStats* getSpecificStats() const;

    PlanRankingResult extractPlanRankingResult() override;

private:
    void _cacheDependingOnPlan(const CanonicalQuery&,
                               MultiPlanStage& mps,
                               std::unique_ptr<plan_ranker::PlanRankingDecision>,
                               std::vector<plan_ranker::CandidatePlan>&);

    std::unique_ptr<MultiPlanStage> _multiplanStage;
    boost::optional<PlanExplainerData> _maybeExplainData;
};

/**
 * Picks the best plan for each $or branch and using caching callbacks to cachek either whole plan
 * or each branch in the classic plan cache.
 */
class SubPlanner final : public DeferredEngineChoicePlannerInterface {
public:
    SubPlanner(PlannerData plannerData);

    PlanRankingResult extractPlanRankingResult() override;

private:
    std::unique_ptr<SubplanStage> _subPlanStage;
};

/**
 * Wrapper planner that performs engine selection after extracting the planning result from an
 * inner planner. This ensures engine selection happens in the planning phase, so that errors (e.g.
 * NoQueryExecutionPlans from invalid query settings on secondary collections) are caught by
 * retryMakePlanner's fallback mechanism.
 */
class EngineSelectionPlanner final : public PlannerInterface {
public:
    EngineSelectionPlanner(std::unique_ptr<PlannerInterface> innerPlanner,
                           OperationContext* opCtx,
                           CanonicalQuery* cq,
                           Pipeline* pipeline,
                           const MultipleCollectionAccessor& collections);

    PlanRankingResult extractPlanRankingResult() override;

private:
    PlanRankingResult _result;
};

}  // namespace mongo::exec_deferred_engine_choice
