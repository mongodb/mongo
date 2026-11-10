// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"

#include <memory>
#include <variant>
#include <vector>

// Looks up join plans in the join plan cache and inserts newly optimized plans into it.
namespace mongo::join_ordering {

/** State carried from a join plan cache miss to the insert after the trial. */
struct JoinPlanCacheMiss {
    JoinPlanCacheKey key;
    // Snapshot of the join-graph collections taken at the miss, before sampling can yield.
    std::vector<CollectionTag> collectionTags;
};

/** A cached entry validated against the current catalog. */
struct JoinPlanCacheHit {
    // TODO(SERVER-136738): use the key to replace the entry when replanning on a hit.
    JoinPlanCacheKey key;
    std::shared_ptr<JoinPlanCacheEntry> entry;
};

using JoinPlanCacheLookupResult = std::variant<JoinPlanCacheHit, JoinPlanCacheMiss>;

/** Whether a query may consult and populate the join plan cache. */
bool canUseJoinPlanCache(const QueryKnobConfiguration& qkc, bool hasHint, bool isExplain);

/** Returns a validated hit, or a miss after evicting a stale entry and snapshotting the tags. */
JoinPlanCacheLookupResult lookupInJoinPlanCache(OperationContext* opCtx,
                                                const MultipleCollectionAccessor& graphOnlyMca,
                                                const AggJoinModel& model,
                                                const AvailableIndexes& perCollIdxs,
                                                const query_settings::QuerySettings& querySettings);

/** Increments the join plan cache hits counter. */
void recordJoinPlanCacheHit();

/** Whether a plan that ran its baseline trial is inserted into the join plan cache, and why not. */
enum class JoinPlanCacheInsertResult {
    kInsert,
    // An index DDL changed a join-graph collection during planning or the trial.
    kSkipCollectionsChanged,
    // The trial stash hit its byte cap, so the baseline covers less than the full trial window.
    kSkipOverStashMemoryLimit,
    // The baseline exceeded 'internalQueryJoinPlanCacheMaxReadsAndIntermediateRows'.
    kSkipOverThreshold,
};

/** Whether every access plan can be cached; an EOF plan carries no 'SolutionCacheData'. */
bool canCacheAccessPlans(const SingleTableAccessPlansResult& singleTableAccess);

/** Makes the uncapped tracker attached to a plan's baseline trial. */
std::unique_ptr<TrialRunTracker> makeBaselineTracker();

/** Reads a plan's baseline from its trial tracker and the number of results it buffered. */
JoinPlanBaseline baselineFromTrial(const TrialRunTracker& tracker, size_t numResults);

/** A trialed join plan and the baseline read from its tracker before the candidate is moved. */
struct JoinTrialResult {
    sbe::plan_ranker::CandidatePlan candidate;
    JoinPlanBaseline baseline;
};

/**
 * Prepares an SBE plan, and runs it as a trial of up to 'maxNumResults' documents with 'tracker'
 * attached, buffering the results into the returned candidate and returning its baseline alongside.
 * Throws if the plan fails during the trial.
 */
JoinTrialResult runJoinTrial(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& mca,
    const CanonicalQuery& baseCQ,
    PlanYieldPolicySBE* yieldPolicy,
    std::unique_ptr<QuerySolution> soln,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> planStagesAndData,
    std::unique_ptr<TrialRunTracker> tracker,
    size_t maxNumResults);

/** Whether a plan that ran its baseline trial can be inserted into the join plan cache. */
JoinPlanCacheInsertResult checkCanInsertIntoJoinPlanCache(bool collectionsUnchanged,
                                                          bool hitStashMemoryLimit,
                                                          const JoinPlanBaseline& baseline,
                                                          size_t maxReadsAndIntermediateRows);

/** Inserts the winning plan of a miss into the cache, or records and logs why it was not. */
void tryInsertIntoJoinPlanCache(OperationContext* opCtx,
                                JoinPlanCacheMiss miss,
                                const MultipleCollectionAccessor& graphOnlyMca,
                                const AggJoinModel& model,
                                const AvailableIndexes& perCollIdxs,
                                const Pipeline& pipeline,
                                std::unique_ptr<CachedJoinPlan> cachedJoinPlan,
                                NodeId baseNode,
                                const JoinPlanBaseline& baseline,
                                bool hitStashMemoryLimit,
                                size_t maxReadsAndIntermediateRows);

}  // namespace mongo::join_ordering
