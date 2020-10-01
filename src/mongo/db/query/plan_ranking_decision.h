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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {
/**
 * Information about why a plan was picked to be the best. Data here is placed into the cache and
 * used to compare expected performance with actual.
 */
struct PlanRankingDecision {
    PlanRankingDecision() {}

    /**
     * Make a deep copy.
     */
    std::unique_ptr<PlanRankingDecision> clone() const {
        auto decision = std::make_unique<PlanRankingDecision>();
        for (size_t i = 0; i < stats.size(); ++i) {
            PlanStageStats* s = stats[i].get();
            invariant(s);
            decision->stats.push_back(std::unique_ptr<PlanStageStats>{s->clone()});
        }
        decision->scores = scores;
        decision->candidateOrder = candidateOrder;
        decision->failedCandidates = failedCandidates;
        return decision;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return  // Add size of each element in 'stats' vector.
            container_size_helper::estimateObjectSizeInBytes(
                stats, [](const auto& stat) { return stat->estimateObjectSizeInBytes(); }, true) +
            // Add size of each element in 'candidateOrder' vector.
            container_size_helper::estimateObjectSizeInBytes(candidateOrder) +
            // Add size of each element in 'failedCandidates' vector.
            container_size_helper::estimateObjectSizeInBytes(failedCandidates) +
            // Add size of each element in 'scores' vector.
            container_size_helper::estimateObjectSizeInBytes(scores) +
            // Add size of the object.
            sizeof(*this);
    }

    // Stats of all plans sorted in descending order by score.
    // Owned by us.
    std::vector<std::unique_ptr<PlanStageStats>> stats;

    // The "goodness" score corresponding to 'stats'.
    // Sorted in descending order.
    std::vector<double> scores;

    // Ordering of original plans in descending of score.
    // Filled in by PlanRanker::pickBestPlan(candidates, ...)
    // so that candidates[candidateOrder[0]] refers to the best plan
    // with corresponding cores[0] and stats[0]. Runner-up would be
    // candidates[candidateOrder[1]] followed by
    // candidates[candidateOrder[2]], ...
    //
    // Contains only non-failing plans.
    std::vector<size_t> candidateOrder;

    // Contains the list of original plans that failed.
    //
    // Like 'candidateOrder', the contents of this array are indices into the 'candidates' array.
    std::vector<size_t> failedCandidates;

    // Whether two plans tied for the win.
    //
    // Reading this flag is the only reliable way for callers to determine if there was a tie,
    // because the scores kept inside the PlanRankingDecision do not incorporate the EOF bonus.
    bool tieForBest = false;
};
}  // namespace mongo
