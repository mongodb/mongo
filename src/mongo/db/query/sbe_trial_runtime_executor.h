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

#include "mongo/db/operation_context.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/util/assert_util_core.h"

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
                         const AllIndicesRequiredChecker& indexExistenceChecker)
        : _opCtx(opCtx),
          _collections(collections),
          _cq(cq),
          _yieldPolicy(yieldPolicy),
          _indexExistenceChecker(indexExistenceChecker) {
        invariant(_opCtx);
    }

    /**
     * Fetches a next document from the given plan stage tree and the loaded document is placed into
     * the candidate's plan result queue.
     *
     * Returns true if a document was fetched, and false if the plan stage tree reached EOF, an
     * exception was thrown or the plan stage tree returned maxNumResults documents.
     *
     * If the plan stage throws a 'QueryExceededMemoryLimitNoDiskUseAllowed', it will be caught and
     * the 'candidate->status' will be set. This failure is considered recoverable, as another
     * candidate plan may require less memory, or may not contain a stage requiring spilling to disk
     * at all.
     */
    static bool fetchNextDocument(plan_ranker::CandidatePlan* candidate, size_t maxNumResults);

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
    // The following data members are not owned.
    OperationContext* const _opCtx;
    const MultipleCollectionAccessor& _collections;
    const CanonicalQuery& _cq;
    PlanYieldPolicySBE* const _yieldPolicy;
    const AllIndicesRequiredChecker& _indexExistenceChecker;
};

}  // namespace mongo::sbe
