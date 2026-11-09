// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

namespace mongo::classic_runtime_planner_for_sbe {

/**
 * Data that any runtime planner needs to perform the planning.
 */
struct PlannerDataForSBE final : public PlannerData {
    PlannerDataForSBE(
        OperationContext* opCtx,
        CanonicalQuery* cq,
        std::unique_ptr<WorkingSet> workingSet,
        const MultipleCollectionAccessor& collections,
        // To be shared between all instances of this type and the prepare helper creating them.
        std::shared_ptr<QueryPlannerParams> plannerParams,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        boost::optional<size_t> cachedPlanHash,
        std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy)
        : PlannerData(opCtx,
                      cq,
                      std::move(workingSet),
                      collections,
                      std::move(plannerParams),
                      yieldPolicy,
                      cachedPlanHash),
          sbeYieldPolicy(std::move(sbeYieldPolicy)) {}

    PlannerDataForSBE(PlannerData plannerData,
                      const MultipleCollectionAccessor& collections,
                      std::shared_ptr<QueryPlannerParams> plannerParams,
                      std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy)
        : PlannerDataForSBE(plannerData.opCtx,
                            plannerData.cq,
                            std::move(plannerData.workingSet),
                            collections,
                            std::move(plannerParams),
                            plannerData.yieldPolicy,
                            plannerData.cachedPlanHash,
                            std::move(sbeYieldPolicy)) {}

    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy;
    boost::optional<size_t> cachedPlanSolutionHash = boost::none;
};

class PlannerBase : public PlannerInterface {
public:
    PlannerBase(PlannerDataForSBE plannerData, PlanSelectionStrategy planSelectionStrategy);

protected:
    std::unique_ptr<PlanYieldPolicySBE> extractSbeYieldPolicy() {
        return std::move(_plannerData.sbeYieldPolicy);
    }

    boost::optional<size_t> cachedPlanHash() const {
        return _plannerData.cachedPlanHash;
    }

    std::shared_ptr<QueryPlannerParams> extractPlannerParams() {
        return std::move(_plannerData.plannerParams);
    }

    PlanSelectionStrategy planSelectionStrategy() const {
        return _planSelectionStrategy;
    }

private:
    PlannerDataForSBE _plannerData;
    PlanSelectionStrategy _planSelectionStrategy;
};

/**
 * Interface for "PlannerGenerators" which, given a cache entry, generates a PlannerInterface
 * plan depending on the result of the cache entry's trial period.
 *
 * The lifetime of retrieving a plan from the cache is as follows:
 *
 *                                     ---->  ValidCandidatePlanner---------->
 *                                    /                                       \
 * Cache Entry --> PlannerGenerator ->  --> SingleSolutionPassthroughPlanner -> --> PlanExecutor
 *                                    \                                       /
 *                                     ----------> MultiPlanner ------------->
 */
class PlannerGeneratorFromCacheEntry {
public:
    virtual ~PlannerGeneratorFromCacheEntry() = default;

    /**
     * Runs the trial period and generates a planner.
     */
    virtual std::unique_ptr<PlannerInterface> makePlanner() = 0;
};

/**
 * PlannerGenerator for classic cache entries. This generates an SBE plan using the given
 * PlannerData and winning QSN tree.
 */
class PlannerGeneratorFromClassicCacheEntry : public PlannerGeneratorFromCacheEntry {
public:
    PlannerGeneratorFromClassicCacheEntry(PlannerDataForSBE plannerData,
                                          std::unique_ptr<QuerySolution> qs,
                                          boost::optional<size_t> decisionReads);

    /**
     * Executes the trial period and returns a Planner implementation depending on the result.
     */
    std::unique_ptr<PlannerInterface> makePlanner() override;

    /**
     * Test-only helper for swapping in a mock SBE plan for the one that was built from the cache
     * entry.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void setSbePlan_forTest(std::unique_ptr<sbe::PlanStage> sbePlan,
                                                            stage_builder::PlanStageData data) {
        _sbePlan = std::move(sbePlan);
        _planStageData = std::move(data);
    }

private:
    PlannerDataForSBE _plannerData;
    std::unique_ptr<QuerySolution> _solution;
    boost::optional<size_t> _decisionReads;

    std::unique_ptr<sbe::PlanStage> _sbePlan;
    boost::optional<stage_builder::PlanStageData> _planStageData;
};

/**
 * Helper function which create a PlannerGenerator of the appropriate type and immediately call
 * makePlanner(). These functions takes the data stored in a cache entry and turn it into a
 * PlannerInterface object, which can then be turned into a runnable PlanExecutor.
 */
std::unique_ptr<PlannerInterface> makePlannerForClassicCacheEntry(
    PlannerDataForSBE plannerData,
    std::unique_ptr<QuerySolution> qs,
    size_t cachedSolutionHash,
    boost::optional<size_t> decisionWorks);

}  // namespace mongo::classic_runtime_planner_for_sbe
