// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo {
class Collection;
class CollectionPtr;

namespace trial_period {

struct TrialPhaseConfig {
    // How many works to give each plan during the trial period.
    size_t maxNumWorksPerPlan;
    // How many results per plan are we targeting to retrieve during the trial period.
    // If a plan returns this many results, we can stop the trial period early.
    size_t targetNumResults;

    // True if this trial phase uses a capped subset of the total work budget for the query,
    // with the expectation that the same multi-planner may be resumed in a later trial phase.
    bool isCappedTrialPhase{false};
};

/**
 * Returns the number of times that we are willing to work a plan during a trial period.
 *
 * Calculated with the following formula, where "|collection|" denotes the approximate number of
 * documents in the collection:
 *
 *   max(maxWorksParam, collFraction * |collection|)
 */
size_t getTrialPeriodMaxWorks(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              int maxWorksParam,
                              double collFraction);

/**
 * Returns the max number of documents which we should allow any plan to return during the
 * trial period. As soon as any plan hits this number of documents, the trial period ends.
 */
size_t getTrialPeriodNumToReturn(const CanonicalQuery& query);

/**
 * Returns the fraction of the collection that we are allowed to scan for each candidate plan.
 */
double getCollFractionPerCandidatePlan(const CanonicalQuery& query, size_t numSolutions);
}  // namespace trial_period
}  // namespace mongo
