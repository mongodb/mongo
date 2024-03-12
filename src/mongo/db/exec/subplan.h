/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/requires_all_indices_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {

class OperationContext;

/**
 * The SubplanStage is used for rooted $or queries. It plans each clause of the $or
 * individually, and then creates an overall query plan based on the winning plan from
 * each clause.
 *
 * Uses the MultiPlanStage in order to rank plans for the individual clauses.
 *
 * Notes on caching strategy:
 *
 *   --Interaction with the plan cache is done on a per-clause basis. For a given clause C,
 *   if there is a plan in the cache for shape C, then C is planned using the index tags
 *   obtained from the plan cache entry. If no cached plan is found for C, then a MultiPlanStage
 *   is used to determine the best plan for the clause; unless there is a tie between multiple
 *   candidate plans, the winner is inserted into the plan cache and used to plan subsequent
 *   executions of C. These subsequent executions of shape C could be either as a clause in
 *   another rooted $or query, or shape C as its own query.
 *
 *   --Plans for entire rooted $or queries are neither written to nor read from the plan cache.
 */
class SubplanStage final : public RequiresAllIndicesStage {
public:
    SubplanStage(ExpressionContext* expCtx,
                 VariantCollectionPtrOrAcquisition collection,
                 WorkingSet* ws,
                 CanonicalQuery* cq,
                 PlanCachingMode cachingMode = PlanCachingMode::AlwaysCache);

    static bool canUseSubplanning(const CanonicalQuery& query);
    static bool needsSubplanning(const CanonicalQuery& query) {
        return internalQueryPlanOrChildrenIndependently.load() &&
            SubplanStage::canUseSubplanning(query);
    }

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SUBPLAN;
    }

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

    /**
     * Selects a plan using subplanning. First uses the query planning results from
     * planSubqueries() and the multi plan stage to select the best plan for each branch.
     *
     * If this effort fails, then falls back on planning the whole query normally rather
     * then planning $or branches independently.
     *
     * If 'shouldConstructClassicExecutableTree' is true, builds a classic executable tree and
     * appends it to the stage's children. If 'shouldConstructClassicExecutableTree' is false, it
     * means we are using the sub planner for SBE queries, and do not need to build a classic
     * executable tree. 'shouldConstructClassicExecutableTree' is true by default.
     *
     * If 'yieldPolicy' is non-NULL, then all locks may be yielded in between round-robin
     * works of the candidate plans. By default, 'yieldPolicy' is NULL and no yielding will
     * take place.
     *
     * Returns a non-OK status if query planning fails. In particular, this function returns
     * ErrorCodes::QueryPlanKilled if the query plan was killed during a yield, or
     * ErrorCodes::MaxTimeMSExpired if the operation has exceeded its time limit.
     */
    Status pickBestPlan(const QueryPlannerParams& plannerParams,
                        PlanYieldPolicy* yieldPolicy,
                        bool shouldConstructClassicExecutableTree = true);

    //
    // For testing.
    //

    /**
     * Returns true if the i-th branch was planned by retrieving a cached solution,
     * otherwise returns false.
     */
    bool branchPlannedFromCache(size_t i) const {
        invariant(i < _branchPlannedFromCache.size());
        return _branchPlannedFromCache[i];
    }

    /**
     * Provide access to the query solution for our composite solution. Does not relinquish
     * ownership.
     */
    QuerySolution* compositeSolution() const {
        return _compositeSolution.get();
    }

    /**
     * Extracts the best query solution. If the sub planner falls back to the multi planner,
     * extracts the best solution from the multi planner, otherwise extracts the composite solution.
     */
    std::unique_ptr<QuerySolution> extractBestWholeQuerySolution() {
        if (usesMultiplanning()) {
            return multiPlannerStage()->extractBestSolution();
        }
        return std::move(_compositeSolution);
    }

    /**
     * Returns true if the sub planner fell back to multiplanning.
     */
    bool usesMultiplanning() const {
        return _usesMultiplanning;
    }

    /**
     * Returns the MultiPlan stage.
     */
    MultiPlanStage* multiPlannerStage() {
        tassert(8524100,
                "The sub planner stage should fall back to the multi planner.",
                _usesMultiplanning);
        return static_cast<MultiPlanStage*>(child().get());
    }


private:
    /**
     * Used as a fallback if subplanning fails. Helper for pickBestPlan().
     */
    Status choosePlanWholeQuery(const QueryPlannerParams& plannerParams,
                                PlanYieldPolicy* yieldPolicy,
                                bool shouldConstructClassicExecutableTree);

    // Not owned here.
    WorkingSet* _ws;

    // Not owned here.
    CanonicalQuery* _query;

    // If we successfully create a "composite solution" by planning each $or branch
    // independently, that solution is owned here.
    std::unique_ptr<QuerySolution> _compositeSolution;

    // Indicates whether i-th branch of the rooted $or query was planned from a cached solution.
    std::vector<bool> _branchPlannedFromCache;

    PlanCachingMode _planCachingMode;

    // Indicates whether the sub planner has fallen back to multi planning.
    bool _usesMultiplanning = false;
};
}  // namespace mongo
