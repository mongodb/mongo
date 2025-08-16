/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/none.hpp>

namespace mongo {
class MultiPlanStage;
}

namespace mongo::plan_cache_util {

/**
 * Builds "DebugInfo" for storing in the classic plan cache.
 */
plan_cache_debug_info::DebugInfo buildDebugInfo(
    const CanonicalQuery& query, std::unique_ptr<const plan_ranker::PlanRankingDecision> decision);

/**
 * Builds "DebugInfoSBE" for storing in the SBE plan cache. Pre-computes necessary debugging
 * information to build "PlanExplainerSBE" when recoverying the cached SBE plan from the cache.
 */
plan_cache_debug_info::DebugInfoSBE buildDebugInfo(const QuerySolution* solution);

/**
 * Updates the classic plan cache from candidates generated using classic planning, but with the
 * intent of executing the query in SBE. If the query is not a type that can be cached, does
 * nothing. It is the caller's responsibility to compute a 'numReads' value to be stored in the
 * cache entry indicating the expected number of reads the SBE plan requires.
 */
void updateClassicPlanCacheFromClassicCandidatesForSbeExecution(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CanonicalQuery& query,
    NumReads numReads,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates);

/**
 * Updates the classic plan cache from candidates generated using classic planning, with the intent
 * of executing it in classic. If the query is not a type that can be cached, does nothing. This
 * uses the 'works' value provided in 'ranking' as the works value to store in the cache.
 */
void updateClassicPlanCacheFromClassicCandidatesForClassicExecution(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CanonicalQuery& query,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates);

/**
 * Caches the best candidate execution plan for 'query' in SBE plan cache, chosen from the given
 * 'candidates' based on the 'ranking' decision, if the 'query' is of a type that can be cached.
 * Otherwise, does nothing.
 *
 * The 'cachingMode' specifies whether the query should be:
 *    * Always cached.
 *    * Never cached.
 *    * Cached, except in certain special cases.
 */
void updateSbePlanCacheWithNumReads(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const CanonicalQuery& query,
    NumReads nReads,
    const std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>& sbePlanAndData,
    const QuerySolution* winningSolution);

/**
 * Caches the plan 'root' along with its accompanying 'data' if the 'query' is of a type that can be
 * cached. Otherwise, does nothing.
 *
 * The given plan will be "pinned" to the cache and will not be subject to replanning. Once put into
 * the cache, the plan immediately becomes "active".
 */
void updateSbePlanCacheWithPinnedEntry(OperationContext* opCtx,
                                       const MultipleCollectionAccessor& collections,
                                       const CanonicalQuery& query,
                                       const QuerySolution& solution,
                                       const sbe::PlanStage& root,
                                       stage_builder::PlanStageData stageData);


/**
 * A function object compatible with 'MultiPlanStage::OnPickBestPlan' which does nothing, leaving
 * the plan cache unaltered.
 */
struct NoopPlanCacheWriter {
    void operator()(const CanonicalQuery&,
                    MultiPlanStage& mps,
                    std::unique_ptr<plan_ranker::PlanRankingDecision>,
                    std::vector<plan_ranker::CandidatePlan>&) const {}
};

/**
 * A function object which, when invoked, updates the classic plan cache entry for query 'cq' based
 * on the multi-planning results described by 'ranking' and 'candidates'.
 *
 * Does nothing if the query is not eligible for caching or the winning plan is illegal to cache.
 */
struct ClassicPlanCacheWriter {
    ClassicPlanCacheWriter(OperationContext* opCtx,
                           const VariantCollectionPtrOrAcquisition& collection,
                           bool executeInSbe)
        : _opCtx(opCtx), _collection(collection), _executeInSbe(executeInSbe) {}

    void operator()(const CanonicalQuery& cq,
                    MultiPlanStage& mps,
                    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                    std::vector<plan_ranker::CandidatePlan>& candidates) const;

protected:
    OperationContext* _opCtx;
    VariantCollectionPtrOrAcquisition _collection;
    bool _executeInSbe;
};

/**
 * A function object which when invoked might update the classic plan cache. Whether the classic
 * plan cache entry is written to depends on the following:
 *  - Whether the query is a type that can be cached.
 *  - Whether the winning plan is legal to cache.
 *  - The 'Mode' configured by the caller. This 'Mode' configuration is what distinguishes this
 *    class from the simpler 'ClassicPlanCacheWriter' above.
 */
class ConditionalClassicPlanCacheWriter : public ClassicPlanCacheWriter {
public:
    enum class Mode {
        // Always write a cache entry for the winning plan to the plan cache, overwriting any
        // previously existing cache entry for the query shape.
        AlwaysCache,

        // Write a cache entry for the query shape *unless* we encounter one of the following edge
        // cases:
        //  - Two or more plans tied for the win.
        //  - The winning plan returned zero query results during the plan ranking trial period.
        SometimesCache,

        // Do not write to the plan cache.
        NeverCache,
    };

    static Mode alwaysOrNeverCacheMode(bool shouldCache) {
        return shouldCache ? Mode::AlwaysCache : Mode::NeverCache;
    }

    ConditionalClassicPlanCacheWriter(Mode planCachingMode,
                                      OperationContext* opCtx,
                                      const VariantCollectionPtrOrAcquisition& collection,
                                      bool executeInSbe)
        : ClassicPlanCacheWriter(opCtx, collection, executeInSbe),
          _planCachingMode{planCachingMode} {}

    void operator()(const CanonicalQuery& cq,
                    MultiPlanStage& mps,
                    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                    std::vector<plan_ranker::CandidatePlan>& candidates) const;

protected:
    bool shouldCacheBasedOnCachingMode(
        const CanonicalQuery& cq,
        const plan_ranker::PlanRankingDecision& ranking,
        const std::vector<plan_ranker::CandidatePlan>& candidates) const;

    const Mode _planCachingMode;
};

// This function computes the value of the "reads" metric for the winning plan using the specified
// 'stats'. This function will always return a positive value.
NumReads computeNumReadsFromStats(const PlanStageStats& stats,
                                  const plan_ranker::PlanRankingDecision& ranking);
}  // namespace mongo::plan_cache_util
