// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/container_size_helper.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::plan_ranker {
struct StatsDetails {
    // Execution stats for each candidate plan sorted in descending order by score.
    std::vector<std::unique_ptr<PlanStageStats>> candidatePlanStats;
};

/**
 * Information about why a plan was picked to be the best.  Data here is placed into the cache
 * and used to compare expected performance with actual.
 */
struct PlanRankingDecision {
    PlanRankingDecision() {}

    /**
     * Copy constructor performs deep copy.
     */
    PlanRankingDecision(const PlanRankingDecision& ranking) {
        std::vector<std::unique_ptr<PlanStageStats>> copy;
        copy.reserve(ranking.stats.candidatePlanStats.size());
        for (const auto& stats : ranking.stats.candidatePlanStats) {
            invariant(stats);
            copy.emplace_back(stats->clone());
        }
        stats = StatsDetails{std::move(copy)};
        scores = ranking.scores;
        candidateOrder = ranking.candidateOrder;
        failedCandidates = ranking.failedCandidates;
    }

    PlanRankingDecision(PlanRankingDecision&& ranking) = default;

    /**
     * Make a deep copy by calling its copy constructor.
     */
    std::unique_ptr<PlanRankingDecision> clone() const {
        return std::make_unique<PlanRankingDecision>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return  // Add size of 'stats' instance.
            container_size_helper::estimateObjectSizeInBytes(
                stats.candidatePlanStats,
                [](auto&& stat) { return stat->estimateObjectSizeInBytes(); },
                true) +
            // Add size of each element in 'candidateOrder' vector.
            container_size_helper::estimateObjectSizeInBytes(candidateOrder) +
            // Add size of each element in 'failedCandidates' vector.
            container_size_helper::estimateObjectSizeInBytes(failedCandidates) +
            // Add size of each element in 'scores' vector.
            container_size_helper::estimateObjectSizeInBytes(scores) +
            // Add size of the object.
            sizeof(*this);
    }

    /*
     * Returns true if there are at least two possible plans, and at least the top two plans
     * have the same scores.
     */
    bool tieForBest() const {
        if (scores.size() > 1) {
            const double epsilon = 1e-10;
            return (std::abs(scores[0] - scores[1]) < epsilon);
        }
        return false;
    }

    // Execution stats details for each candidate plan.
    StatsDetails stats;

    // The "goodness" score corresponding to 'stats'.
    // Sorted in descending order.
    std::vector<double> scores;

    // Ordering of original plans in descending of score.
    // Filled in by PlanScorer::pickBestPlan(candidates, ...)
    // so that candidates[candidateOrder[0]] refers to the best plan
    // with corresponding cores[0] and stats[0]. Runner-up would be
    // candidates[candidateOrder[1]] followed by
    // candidates[candidateOrder[2]], ...
    //
    // Contains only non-failing plans.
    std::vector<size_t> candidateOrder;

    // Contains the list of original plans that failed.
    //
    // Like 'candidateOrder', the contents of this array are indicies into the 'candidates' array.
    std::vector<size_t> failedCandidates;
};
}  // namespace mongo::plan_ranker
