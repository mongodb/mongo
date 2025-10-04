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

#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"

namespace mongo::classic_runtime_planner_for_sbe {

/**
 * Data that any runtime planner needs to perform the planning.
 */
struct PlannerDataForSBE final : public PlannerData {
    PlannerDataForSBE(OperationContext* opCtx,
                      CanonicalQuery* cq,
                      std::unique_ptr<WorkingSet> workingSet,
                      const MultipleCollectionAccessor& collections,
                      std::unique_ptr<QueryPlannerParams> plannerParams,
                      PlanYieldPolicy::YieldPolicy yieldPolicy,
                      boost::optional<size_t> cachedPlanHash,
                      std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
                      bool useSbePlanCache)
        : PlannerData(opCtx,
                      cq,
                      std::move(workingSet),
                      collections,
                      std::move(plannerParams),
                      yieldPolicy,
                      cachedPlanHash),
          sbeYieldPolicy(std::move(sbeYieldPolicy)),
          useSbePlanCache(useSbePlanCache) {}

    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy;

    // If true, runtime planners will use the SBE plan cache rather than the classic plan cache.
    const bool useSbePlanCache;

    // Used to determine whether a replanned plan is the same as the plan that has already been
    // cached. Slightly different from PlannerData::cachedPlanHash because that member can only be
    // set if there is a solution in the cache as well, but plans in the SBE plan cache only store
    // the PlanStage root and the solution hash, not the solution itself.
    boost::optional<size_t> cachedPlanSolutionHash = boost::none;
};

class PlannerBase : public PlannerInterface {
public:
    PlannerBase(PlannerDataForSBE plannerData);

protected:
    /**
     * Function that prepares 'sbePlanAndData' for execution and passes the correct arguments to a
     * new instance of PlanExecutorSBE and returns it. Note that the classicRuntimePlannerStage is
     * only passed to PlanExecutorSBE so that it can be plumbed through to a PlanExplainer to
     * generate the correct explain output when using the classic multiplanner with SBE.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> prepareSbePlanExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery,
        std::unique_ptr<QuerySolution> solution,
        std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> sbePlanAndData,
        bool isFromPlanCache,
        boost::optional<size_t> cachedPlanHash,
        std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage);

    std::unique_ptr<QuerySolution> extendSolutionWithPipeline(
        std::unique_ptr<QuerySolution> solution);

    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> prepareSbePlanAndData(
        const QuerySolution& solution, boost::optional<std::string> replanReason = boost::none);

    OperationContext* opCtx() {
        return _plannerData.opCtx;
    }

    CanonicalQuery* cq() {
        return _plannerData.cq;
    }

    const CanonicalQuery* cq() const {
        return _plannerData.cq;
    }

    const MultipleCollectionAccessor& collections() const {
        return _plannerData.collections;
    }

    PlanYieldPolicy::YieldPolicy yieldPolicy() const {
        return _plannerData.yieldPolicy;
    }

    PlanYieldPolicySBE* sbeYieldPolicy() {
        return _plannerData.sbeYieldPolicy.get();
    }

    std::unique_ptr<PlanYieldPolicySBE> extractSbeYieldPolicy() {
        return std::move(_plannerData.sbeYieldPolicy);
    }

    size_t plannerOptions() const {
        return _plannerData.plannerParams->mainCollectionInfo.options;
    }

    boost::optional<size_t> cachedPlanHash() const {
        return _plannerData.cachedPlanHash;
    }

    // See comments in PlannerDataForSBE about why this is necessary and distinct from
    // cachedPlanHash.
    boost::optional<size_t> cachedPlanSolutionHash() const {
        return _plannerData.cachedPlanSolutionHash;
    }

    WorkingSet* ws() {
        return _plannerData.workingSet.get();
    }

    std::unique_ptr<WorkingSet> extractWs() {
        return std::move(_plannerData.workingSet);
    }

    const QueryPlannerParams& plannerParams() const {
        return *_plannerData.plannerParams;
    }

    PlannerDataForSBE extractPlannerData() {
        return std::move(_plannerData);
    }

    bool useSbePlanCache() const {
        return _plannerData.useSbePlanCache;
    }

private:
    PlannerDataForSBE _plannerData;
};

/**
 * Trivial planner that just creates an executor when there is only one QuerySolution present.
 */
class SingleSolutionPassthroughPlanner final : public PlannerBase {
public:
    SingleSolutionPassthroughPlanner(PlannerDataForSBE plannerData,
                                     std::unique_ptr<QuerySolution> solution,
                                     boost::optional<std::string> replanReason = boost::none);

    SingleSolutionPassthroughPlanner(
        PlannerDataForSBE plannerData,
        std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> sbePlanAndData);

    /**
     * Builds and caches SBE plan for the given solution and returns PlanExecutor for it.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override;

private:
    std::unique_ptr<QuerySolution> extendSolutionWithPipelineIfNeeded(
        std::unique_ptr<QuerySolution> solution);

    std::unique_ptr<QuerySolution> _solution;
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> _sbePlanAndData;
    bool _isFromPlanCache;
};

class MultiPlanner final : public PlannerBase {
public:
    MultiPlanner(
        PlannerDataForSBE plannerData,
        std::vector<std::unique_ptr<QuerySolution>> candidatePlans,
        bool shouldWriteToPlanCache,
        const std::function<void()>& incrementReplannedPlanIsCachedPlanCounterCb = []() {},
        boost::optional<std::string> replanReason = boost::none);

    /**
     * Picks the best plan given by the classic engine multiplanner and returns a plan executor. If
     * the planner finished running the best solution during multiplanning, we return the documents
     * and exit, otherwise we pick the best plan and return the SBE plan executor.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override;

private:
    using SbePlanAndData = std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>;

    bool _shouldUseEofOptimization() const;

    // Callback to pass to the 'MultiPlanStage'. Constructs an SBE plan for the winning plan,
    // dealing with the possibility of a pushed-down agg pipeline. If '_shouldWriteToPlanCache' is
    // true, writes the resulting SBE plan to the SBE plan cache.
    void _buildSbePlanAndMaybeCache(const CanonicalQuery&,
                                    MultiPlanStage& mps,
                                    std::unique_ptr<plan_ranker::PlanRankingDecision>,
                                    std::vector<plan_ranker::CandidatePlan>&);

    std::unique_ptr<MultiPlanStage> _multiPlanStage;
    const bool _shouldWriteToPlanCache;
    const std::function<void()>& _incrementReplannedPlanIsCachedPlanCounterCb;
    boost::optional<std::string> _replanReason;

    // The SBE plan is constructed from the callback we pass to the 'MultiPlanStage' so that it can
    // be used to construct a cache entry. Then it gets stashed here so that it can be subsequently
    // used to create an SBE plan executor.
    boost::optional<SbePlanAndData> _sbePlanAndData;

    // Temporary owner of query solution; ownership is surrendered in makeExecutor().
    std::unique_ptr<QuerySolution> _winningSolution;
};

/**
 * Uses the classic sub-planner to construct a plan for a rooted $or query. Subplanning involves
 * selecting an indexed access path independently for each branch of a rooted $or query. See
 * 'SubplanStage' for details.
 *
 * The plan caching strategy differs depending on whether we are using the classic plan cache or the
 * SBE plan cache. When using the classic plan cache, we write a separate plan cache entry for each
 * branch of the $or. This requires threading a callback through the 'SubplanStage' to write to the
 * classic plan cache; the callback will get involved whenever multi-planning for a particular $or
 * branch completes.
 *
 * The SBE plan cache requires that we cache entire plans -- it does not support stitching partial
 * cached plans together to create a composite plan. When the SBE plan cache is on, we therefore do
 * not plumb through a callback as described above. Instead, this class takes on the responsibility
 * of constructing the complete SBE plan for the query and writing it to the SBE plan cache.
 */
class SubPlanner final : public PlannerBase {
public:
    SubPlanner(PlannerDataForSBE plannerData);

    /**
     * Picks the composite solution given by the classic engine subplanner, extends the composite
     * solution with the cq pipeline, creates a pinned plan cache entry containing the resulting SBE
     * plan, and returns a plan executor.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override;

    /**
     * Returns the number of times that an individual $or branch was multi-planned during the
     * subplanning process.
     */
    int numPerBranchMultiplans() const {
        return _numPerBranchMultiplans;
    }

private:
    SubplanStage::PlanSelectionCallbacks makeCallbacks();

    std::unique_ptr<SubplanStage> _subplanStage;
    // Temporary owner of query solution; ownership is surrendered in makeExecutor().
    std::unique_ptr<QuerySolution> _solution;

    int _numPerBranchMultiplans = 0;
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
 * PlannerGenerator for a cache entry retrieved from the SBE plan cache.
 */
class PlannerGeneratorFromSbeCacheEntry : public PlannerGeneratorFromCacheEntry {
public:
    PlannerGeneratorFromSbeCacheEntry(PlannerDataForSBE plannerData,
                                      std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder)
        : _plannerData(std::move(plannerData)), _cachedPlanHolder(std::move(cachedPlanHolder)) {}
    /**
     * Checks if any foreign collections have changed size significantly, and if so, returns a
     * MultiPlanner.  Otherwise executes the trial period and returns a Planner implementation
     * based on the result.
     */
    std::unique_ptr<PlannerInterface> makePlanner() override;

private:
    PlannerDataForSBE _plannerData;
    std::unique_ptr<sbe::CachedPlanHolder> _cachedPlanHolder;
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
    void setSbePlan_forTest(std::unique_ptr<sbe::PlanStage> sbePlan,
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
 * Helper functions which create a PlannerGenerator of the appropriate type and immediately call
 * makePlanner(). These functions takes the data stored in a cache entry and turn it into a
 * PlannerInterface object, which can then be turned into a runnable PlanExecutor.
 */
std::unique_ptr<PlannerInterface> makePlannerForSbeCacheEntry(
    PlannerDataForSBE plannerData,
    std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder,
    size_t cachedSolutionHash);

std::unique_ptr<PlannerInterface> makePlannerForClassicCacheEntry(
    PlannerDataForSBE plannerData,
    std::unique_ptr<QuerySolution> qs,
    size_t cachedSolutionHash,
    boost::optional<size_t> decisionWorks);

}  // namespace mongo::classic_runtime_planner_for_sbe
