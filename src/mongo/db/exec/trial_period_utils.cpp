// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/trial_period_utils.h"

#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"

#include <algorithm>


namespace mongo::trial_period {

double getCollFractionPerCandidatePlan(const CanonicalQuery& query, size_t numSolutions) {
    const double collFraction =
        query.getExpCtx()->getQueryKnobConfiguration().getPlanEvaluationCollFraction();
    const double totalCollFraction =
        query.getExpCtx()->getQueryKnobConfiguration().getPlanTotalEvaluationCollFraction();

    return std::min(collFraction, totalCollFraction / numSolutions);
}

size_t getTrialPeriodMaxWorks(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              int maxWorksParam,
                              double collFraction) {
    size_t numWorks = static_cast<size_t>(maxWorksParam);
    if (collection) {
        numWorks =
            std::max(numWorks, static_cast<size_t>(collFraction * collection->numRecords(opCtx)));
    }

    return numWorks;
}

size_t getTrialPeriodNumToReturn(const CanonicalQuery& query) {
    // Determine the number of results which we will produce during the plan ranking phase before
    // stopping.
    size_t numResults =
        query.getExpCtx()->getQueryKnobConfiguration().getPlanEvaluationMaxResultsForOp();
    if (query.getFindCommandRequest().getLimit()) {
        numResults =
            std::min(static_cast<size_t>(*query.getFindCommandRequest().getLimit()), numResults);
    }

    return numResults;
}
}  // namespace mongo::trial_period
