// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::sbe {

/**
 * TrialRuntimeExecutor provides a method to perform a trial run for the candidate plan by executing
 * each plan and collecting execution stats.
 */
class TrialRuntimeExecutor {
public:
    TrialRuntimeExecutor(OperationContext* opCtx,
                         const MultipleCollectionAccessor& collections,
                         const CanonicalQuery& cq,
                         PlanYieldPolicySBE* yieldPolicy,
                         const AllIndicesRequiredChecker& indexExistenceChecker,
                         size_t stashSizeMaxBytes = std::numeric_limits<size_t>::max())

        : _stashSizeMaxBytes(stashSizeMaxBytes),
          _opCtx(opCtx),
          _collections(collections),
          _cq(cq),
          _yieldPolicy(yieldPolicy),
          _indexExistenceChecker(indexExistenceChecker) {
        tassert(11321204, "opCtx must not be null", _opCtx);
    }

    /**
     * Fetches a next document from the given plan stage tree and the loaded document is placed into
     * the candidate's plan result queue.
     *
     * Returns true if a document was fetched and the trial run should continue. Returns false if
     * one of the conditions to terminate the trial run has been reached:
     * * The plan returned maxNumResults documents.
     * * The trial run tracker reached the limit of kNumResults metric.
     * * The stashed documents reached the memory limit.
     * * The plan reached EOF.
     * * An error has occured.
     *
     * If the plan stage throws a 'QueryExceededMemoryLimitNoDiskUseAllowed', it will be caught and
     * the 'candidate->status' will be set. This failure is considered recoverable, as another
     * candidate plan may require less memory, or may not contain a stage requiring spilling to disk
     * at all.
     */
    bool fetchNextDocument(plan_ranker::CandidatePlan* candidate, size_t maxNumResults);

    /**
     * Prepares the given plan stage tree for execution, attaches it to the operation context and
     * returns two slot accessors for the result and recordId slots. The caller should pass true
     * for 'preparingFromCache' if the SBE plan being prepared is being recovered from the SBE plan
     * cache. The caller should pass 'remoteCursors' for replanning hash lookup in $search query,
     * otherwise keep it nullptr.
     */
    std::pair<sbe::value::SlotAccessor*, sbe::value::SlotAccessor*> prepareExecutionPlan(
        PlanStage* root,
        stage_builder::PlanStageData* data,
        bool preparingFromCache,
        RemoteCursorMap* remoteCursors = nullptr) const;

    /**
     * Wraps prepareExecutionPlan(), checks index validity, and caches outputAccessors.
     */
    void prepareCandidate(plan_ranker::CandidatePlan* candidate, bool preparingFromCache);

    void executeCachedCandidateTrial(plan_ranker::CandidatePlan* candidate, size_t maxNumResults);

private:
    size_t _stashSizeBytes = 0;
    const size_t _stashSizeMaxBytes;

    // The following data members are not owned.
    OperationContext* const _opCtx;
    const MultipleCollectionAccessor& _collections;
    const CanonicalQuery& _cq;
    PlanYieldPolicySBE* const _yieldPolicy;
    const AllIndicesRequiredChecker& _indexExistenceChecker;
};

}  // namespace mongo::sbe
