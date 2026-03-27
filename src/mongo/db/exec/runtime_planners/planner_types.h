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

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Data that any runtime planner needs to perform the planning.
 */
struct PlannerData {
    PlannerData(OperationContext* opCtx,
                CanonicalQuery* cq,
                std::unique_ptr<WorkingSet> workingSet,
                const MultipleCollectionAccessor& collections,
                std::shared_ptr<QueryPlannerParams> plannerParams,
                PlanYieldPolicy::YieldPolicy yieldPolicy,
                boost::optional<size_t> cachedPlanHash)
        : opCtx(opCtx),
          cq(cq),
          workingSet(std::move(workingSet)),
          collections(collections),
          plannerParams(std::move(plannerParams)),
          yieldPolicy(yieldPolicy),
          cachedPlanHash(cachedPlanHash) {}

    PlannerData(const PlannerData&) = delete;
    PlannerData& operator=(const PlannerData&) = delete;
    PlannerData(PlannerData&&) = default;
    PlannerData& operator=(PlannerData&&) = default;

    virtual ~PlannerData() = default;

    OperationContext* opCtx;
    CanonicalQuery* cq;
    std::unique_ptr<WorkingSet> workingSet;
    const MultipleCollectionAccessor& collections;
    // Shared pointer since this is shared across all instances of this type and also
    // prepare helper functions that indeed create this objects.
    std::shared_ptr<QueryPlannerParams> plannerParams;
    PlanYieldPolicy::YieldPolicy yieldPolicy;
    boost::optional<size_t> cachedPlanHash;
};


/**
 * Stores relevant state required to resume executing a partially evaluated PlanStage at a later
 * time.
 *
 * Later, a SingleSolutionPassthroughPlanner can be rebuilt using this.
 *
 * This allows any planning method (multiplanning, CBR fallback, cache trialing) to "stash" the work
 * done, so the caller can create an executor which does not need to repeat work. The classic saved
 * exec state is used to save multiplanning work and classic cached plan trialing work. The SBE
 * saved exec state is used to save SBE cached plan trialing work.
 */
struct ClassicExecState {
    std::unique_ptr<WorkingSet> workingSet;
    std::unique_ptr<PlanStage> root;
};
struct SbeExecState {
    sbe::plan_ranker::CandidatePlan sbeCandidate;
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy;
};
struct SavedExecState {
    SavedExecState(ClassicExecState c) : savedState(std::move(c)) {}
    SavedExecState(SbeExecState s) : savedState(std::move(s)) {}

    template <class T>
    const T* peekExecState() const {
        return std::get_if<T>(&savedState);
    }

    template <class T>
    T* peekExecState() {
        return std::get_if<T>(&savedState);
    }

    template <class T>
    T extractExecState() {
        tassert(11756605,
                "Expected SavedExecState to hold requested execution state",
                std::holds_alternative<T>(savedState));
        return std::get<T>(std::move(savedState));
    }

private:
    std::variant<ClassicExecState, SbeExecState> savedState;
};

struct PlanRankingResult {
    // For the express fast-path, planning will produce an executor.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> expressExecutor;
    // Indicates whether an IDHACK plan was created during planning. This plan will only use the
    // classic engine.
    bool usedIdhack = false;

    std::vector<std::unique_ptr<QuerySolution>> solutions;
    boost::optional<PlanExplainerData> maybeExplainData;

    // True if these plans were chosen without a pre-execution trial run that measured the
    // 'work' metric (for example, selected by a non-multiplanner). Such plans must be
    // run in a pre-execution phase to measure the amount of work done to produce the
    // first batch, so they can be considered for insertion into the classic plan cache.
    bool needsWorksMeasuredForPlanCache = false;

    // Ranker strategies may involve execution; they can return execution-relevant state
    // here, and the caller can choose to resume execution from that point.
    // (e.g., MultiPlanStage may contain spooled results, partially evaluated ixscans, etc.)
    // If none, the caller should consume the provided solution(s) as-is.
    boost::optional<SavedExecState> execState;
    std::shared_ptr<QueryPlannerParams> plannerParams;

    // Hash of the plan for this query that exists in the cache.
    boost::optional<size_t> cachedPlanHash;

    // Populated only if `featureFlagGetExecutorDeferredEngineChoice` is enabled.
    // If a plan was fetched from the plan cache, we need to know which engine to
    // use for trialing, so we call engine selection early and store the result here.
    boost::optional<EngineChoice> engineSelection;
};
}  // namespace mongo
