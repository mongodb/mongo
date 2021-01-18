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

#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_ranker.h"

// Forward declarations.
namespace mongo::sbe::plan_ranker {
std::unique_ptr<mongo::plan_ranker::PlanScorer<PlanStageStats>> makePlanScorer(
    const QuerySolution* solution);
}  // namespace mongo::sbe::plan_ranker

namespace mongo::plan_ranker {
/**
 * A factory function to create a plan scorer for a plan stage stats tree.
 */
std::unique_ptr<PlanScorer<PlanStageStats>> makePlanScorer();

/**
 * Returns a PlanRankingDecision which has the ranking and the information about the ranking
 * process with status OK if everything worked. 'candidateOrder' within the PlanRankingDecision
 * holds indices into candidates ordered by score (winner in first element).
 *
 * Returns an error if there was an issue with plan ranking (e.g. there was no viable plan).
 */
template <typename PlanStageStatsType, typename PlanStageType, typename ResultType, typename Data>
StatusWith<std::unique_ptr<PlanRankingDecision>> pickBestPlan(
    const std::vector<BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates) {
    invariant(!candidates.empty());
    // A plan that hits EOF is automatically scored above
    // its peers. If multiple plans hit EOF during the same
    // set of round-robin calls to work(), then all such plans
    // receive the bonus.
    double eofBonus = 1.0;

    // Get stat trees from each plan.
    std::vector<std::unique_ptr<PlanStageStatsType>> statTrees;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if constexpr (std::is_same_v<PlanStageStatsType, PlanStageStats>) {
            statTrees.push_back(candidates[i].root->getStats());
        } else {
            statTrees.push_back(candidates[i].root->getStats(false /* includeDebugInfo */));
        }
    }

    // Holds (score, candidateIndex).
    // Used to derive scores and candidate ordering.
    std::vector<std::pair<double, size_t>> scoresAndCandidateIndices;
    std::vector<size_t> failed;

    // Compute score for each tree.  Record the best.
    for (size_t i = 0; i < statTrees.size(); ++i) {
        auto explainer = [&]() {
            if constexpr (std::is_same_v<PlanStageStatsType, PlanStageStats>) {
                return plan_explainer_factory::make(candidates[i].root,
                                                    candidates[i].solution->_enumeratorExplainInfo);
            } else {
                static_assert(std::is_same_v<PlanStageStatsType, mongo::sbe::PlanStageStats>);
                return plan_explainer_factory::make(
                    candidates[i].root.get(), &candidates[i].data, candidates[i].solution.get());
            }
        }();

        if (!candidates[i].failed) {
            log_detail::logScoringPlan([&]() { return candidates[i].solution->toString(); },
                                       [&]() {
                                           auto&& [stats, _] = explainer->getWinningPlanStats(
                                               ExplainOptions::Verbosity::kExecStats);
                                           return stats.jsonString(ExtendedRelaxedV2_0_0, true);
                                       },
                                       [&]() { return explainer->getPlanSummary(); },
                                       i,
                                       statTrees[i]->common.isEOF);
            auto scorer = [solution = candidates[i].solution.get()]()
                -> std::unique_ptr<PlanScorer<PlanStageStatsType>> {
                if constexpr (std::is_same_v<PlanStageStatsType, PlanStageStats>) {
                    return makePlanScorer();
                } else {
                    static_assert(std::is_same_v<PlanStageStatsType, mongo::sbe::PlanStageStats>);
                    return sbe::plan_ranker::makePlanScorer(solution);
                }
            }();
            double score = scorer->calculateScore(statTrees[i].get());
            log_detail::logScore(score);
            if (statTrees[i]->common.isEOF) {
                log_detail::logEOFBonus(eofBonus);
                score += 1;
            }

            scoresAndCandidateIndices.push_back(std::make_pair(score, i));
        } else {
            failed.push_back(i);
            log_detail::logFailedPlan([&] { return explainer->getPlanSummary(); });
        }
    }

    // If there isn't a viable plan we should error.
    if (scoresAndCandidateIndices.size() == 0U) {
        return {ErrorCodes::Error(31157),
                "No viable plan was found because all candidate plans failed."};
    }

    // Sort (scores, candidateIndex). Get best child and populate candidate ordering.
    std::stable_sort(scoresAndCandidateIndices.begin(),
                     scoresAndCandidateIndices.end(),
                     [](const auto& lhs, const auto& rhs) {
                         // Just compare score in lhs.first and rhs.first;
                         // Ignore candidate array index in lhs.second and rhs.second.
                         return lhs.first > rhs.first;
                     });

    auto why = std::make_unique<PlanRankingDecision>();

    if constexpr (std::is_same_v<PlanStageStatsType, mongo::sbe::PlanStageStats>) {
        // For SBE, we need to store a serialized winning plan within the ranking decision to be
        // able to included it into the explain output for a cached plan stats, since we cannot
        // reconstruct it from a PlanStageStats tree.
        auto explainer = plan_explainer_factory::make(
            candidates[0].root.get(), &candidates[0].data, candidates[0].solution.get());
        auto&& [stats, _] =
            explainer->getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
        SBEStatsDetails details;
        details.serializedWinningPlan = std::move(stats);
        why->stats = std::move(details);
    } else {
        static_assert(std::is_same_v<PlanStageStatsType, PlanStageStats>);
        why->stats = StatsDetails{};
    }

    // Determine whether plans tied for the win.
    if (scoresAndCandidateIndices.size() > 1U) {
        double bestScore = scoresAndCandidateIndices[0].first;
        double runnerUpScore = scoresAndCandidateIndices[1].first;
        const double epsilon = 1e-10;
        why->tieForBest = std::abs(bestScore - runnerUpScore) < epsilon;
    }

    // Update results in 'why'
    // Stats and scores in 'why' are sorted in descending order by score.
    auto&& stats = why->getStats<PlanStageStatsType>();
    why->failedCandidates = std::move(failed);
    for (size_t i = 0; i < scoresAndCandidateIndices.size(); ++i) {
        double score = scoresAndCandidateIndices[i].first;
        size_t candidateIndex = scoresAndCandidateIndices[i].second;

        // We shouldn't cache the scores with the EOF bonus included, as this is just a
        // tie-breaking measure for plan selection. Plans not run through the multi plan runner
        // will not receive the bonus.
        //
        // An example of a bad thing that could happen if we stored scores with the EOF bonus
        // included:
        //
        //   Let's say Plan A hits EOF, is the highest ranking plan, and gets cached as such. On
        //   subsequent runs it will not receive the bonus. Eventually the plan cache feedback
        //   mechanism will evict the cache entry - the scores will appear to have fallen due to
        //   the missing EOF bonus.
        //
        // This raises the question, why don't we include the EOF bonus in scoring of cached plans
        // as well? The problem here is that the cached plan runner always runs plans to completion
        // before scoring. Queries that don't get the bonus in the multi plan runner might get the
        // bonus after being run from the plan cache.
        if (statTrees[candidateIndex]->common.isEOF) {
            score -= eofBonus;
        }

        stats.candidatePlanStats.push_back(std::move(statTrees[candidateIndex]));
        why->scores.push_back(score);
        why->candidateOrder.push_back(candidateIndex);
    }
    for (auto& i : why->failedCandidates) {
        stats.candidatePlanStats.push_back(std::move(statTrees[i]));
    }

    return StatusWith<std::unique_ptr<PlanRankingDecision>>(std::move(why));
}
}  // namespace mongo::plan_ranker
