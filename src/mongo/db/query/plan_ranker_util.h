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

#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_ranker.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
 * Takes a vector of pairs holding (score, planIndex).
 * Returns an iterator pointing to the first non-tying plan, or the end of the vector.
 */
inline std::vector<std::pair<double, size_t>>::iterator findTopTiedPlans(
    std::vector<std::pair<double, size_t>>& plans) {
    return std::find_if(plans.begin(), plans.end(), [&plans](const auto& plan) {
        return plan.first != plans[0].first;
    });
}

/**
 * Holds information about tie breaking heuristic bonuses. It is used to update candidate plan score
 * and log the bonuses.
 */
struct TieBreakingScores {
    TieBreakingScores(bool isPlanTied, double score)
        : isPlanTied(isPlanTied),
          score(score),
          docsExaminedBonus(0.0),
          indexPrefixBonus(0.0),
          distinctScanBonus(0.0) {}

    double getTotalBonus() const {
        return docsExaminedBonus + indexPrefixBonus + distinctScanBonus;
    }

    const bool isPlanTied;
    const double score;
    double docsExaminedBonus;
    double indexPrefixBonus;
    double distinctScanBonus;
};

/**
 * Apply docs examined tie breaking heuristic and return bonuses in 'scores' list. 'candidates' and
 * 'scores' are synchronized, with the i-th score from 'scores' corresponding to the i-th
 * 'candidate.'
 */
template <typename PlanStageType, typename ResultType, typename Data>
void calcDocsExaminedHeuristicBonus(
    const std::vector<std::pair<double, size_t>>& scoresAndCandidateIndices,
    size_t numberOfTiedPlans,
    const std::vector<BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates,
    const std::vector<size_t>& documentsExamined,
    std::vector<TieBreakingScores>& scores) {
    // The vector tiedPlans holds the number of documents and the plan's index.
    std::vector<std::pair<double, size_t>> tiedPlans{};
    tiedPlans.reserve(numberOfTiedPlans);
    for (size_t i = 0; i < numberOfTiedPlans; ++i) {
        const size_t candidateIndex = scoresAndCandidateIndices[i].second;
        tiedPlans.emplace_back(std::make_pair(documentsExamined[candidateIndex], candidateIndex));
    }

    // Sort top plans by least documents examined, and allocate a bonus to each of the top plans.
    std::stable_sort(tiedPlans.begin(), tiedPlans.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    auto stillTiedPlansEnd = findTopTiedPlans(tiedPlans);
    for (auto topPlan = tiedPlans.begin(); topPlan < stillTiedPlansEnd; ++topPlan) {
        scores[topPlan->second].docsExaminedBonus = kBonusEpsilon;
    }
}

/**
 * Apply best index prefix tie breaking heuristic and return bonuses in 'scores' list. 'candidates'
 * and 'scores' are synchronized, with the i-th score from 'scores' corresponding to the i-th
 * 'candidate.'
 */
template <typename PlanStageType, typename ResultType, typename Data>
void calcIndexPrefixHeuristicBonus(
    const std::vector<std::pair<double, size_t>>& scoresAndCandidateIndices,
    size_t numberOfTiedPlans,
    const std::vector<BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates,
    std::vector<TieBreakingScores>& scores) {
    std::vector<const QuerySolution*> solutions{};
    solutions.reserve(numberOfTiedPlans);

    for (size_t i = 0; i < numberOfTiedPlans; ++i) {
        const size_t candidateIndex = scoresAndCandidateIndices[i].second;
        solutions.emplace_back(candidates[candidateIndex].solution.get());
    }

    auto winIndices = applyIndexPrefixHeuristic(solutions);
    for (auto winIndex : winIndices) {
        const auto candidateIndex = scoresAndCandidateIndices[winIndex].second;
        scores[candidateIndex].indexPrefixBonus += 2 * kBonusEpsilon;
    }
}

/**
 * Boost the score of distinct scan plans in case of a tie.
 */
template <typename PlanStageType, typename ResultType, typename Data>
void calcDistinctScanBonus(
    const std::vector<std::pair<double, size_t>>& scoresAndCandidateIndices,
    size_t numberOfTiedPlans,
    const std::vector<BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates,
    std::vector<TieBreakingScores>& scores) {

    for (size_t i = 0; i < numberOfTiedPlans; ++i) {
        const size_t candidateIndex = scoresAndCandidateIndices[i].second;
        if (candidates[candidateIndex].solution->hasNode(STAGE_DISTINCT_SCAN)) {
            scores[candidateIndex].distinctScanBonus += kBonusEpsilon;
        }
    }
}

/**
 * Apply tie-breaking hearistics and update candidate plan scores.
 */
template <typename PlanStageType, typename ResultType, typename Data>
void addTieBreakingHeuristicsBonuses(
    std::vector<std::pair<double, size_t>>& scoresAndCandidateIndices,
    const std::vector<BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates,
    const std::vector<size_t>& documentsExamined) {
    auto tiedPlansEnd = findTopTiedPlans(scoresAndCandidateIndices);
    int numberOfTiedPlans = std::distance(scoresAndCandidateIndices.begin(), tiedPlansEnd);

    if (numberOfTiedPlans > 1) {
        // Initialize 'scores' list. 'candidates' and 'scores' are synchronized, with the i-th score
        // from 'scores' corresponding to the i-th 'candidate.'
        std::vector<TieBreakingScores> scores{};
        scores.reserve(candidates.size());
        for (size_t i = 0; i < scoresAndCandidateIndices.size(); ++i) {
            scores.emplace_back(/* isPlanTied */ i < static_cast<size_t>(numberOfTiedPlans),
                                /* score */ scoresAndCandidateIndices[i].first);
        }

        calcDocsExaminedHeuristicBonus(
            scoresAndCandidateIndices, numberOfTiedPlans, candidates, documentsExamined, scores);

        calcIndexPrefixHeuristicBonus(
            scoresAndCandidateIndices, numberOfTiedPlans, candidates, scores);

        calcDistinctScanBonus(scoresAndCandidateIndices, numberOfTiedPlans, candidates, scores);

        // Log tie breaking bonuses.
        for (const auto& score : scores) {
            LOGV2_DEBUG(
                8027500, 2, "Tie breaking heuristics", "formula"_attr = [&]() {
                    StringBuilder sb;
                    sb << "isPlanTied: " << score.isPlanTied << ". finalScore("
                       << str::convertDoubleToString(score.score + score.docsExaminedBonus +
                                                     score.indexPrefixBonus)
                       << ") = score(" << str::convertDoubleToString(score.score)
                       << ") + docsExaminedBonus("
                       << str::convertDoubleToString(score.docsExaminedBonus)
                       << ") + indexPrefixBonus("
                       << str::convertDoubleToString(score.indexPrefixBonus)
                       << ") + distinctScanBonus("
                       << str::convertDoubleToString(score.distinctScanBonus);
                    return sb.str();
                }());
        }

        for (auto& scoreAndIndex : scoresAndCandidateIndices) {
            scoreAndIndex.first += scores[scoreAndIndex.second].getTotalBonus();
        }
    }
}

/**
 * Returns a PlanRankingDecision which has the ranking and the information about the ranking
 * process with status OK if everything worked. 'candidateOrder' within the PlanRankingDecision
 * holds indices into candidates ordered by score (winner in first element).
 *
 * Returns an error if there was an issue with plan ranking (e.g. there was no viable plan).
 */
template <typename PlanStageType, typename ResultType, typename Data>
StatusWith<std::unique_ptr<PlanRankingDecision>> pickBestPlan(
    const std::vector<BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates,
    const CanonicalQuery& cq) {
    invariant(!candidates.empty());
    // A plan that hits EOF is automatically scored above
    // its peers. If multiple plans hit EOF during the same
    // set of round-robin calls to work(), then all such plans
    // receive the bonus.
    double eofBonus = 1.0;

    // Get stat trees from each plan.
    std::vector<std::unique_ptr<PlanStageStats>> statTrees;
    statTrees.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        statTrees.push_back(candidates[i].root->getStats());
    }

    // Holds (score, candidateIndex).
    // Used to derive scores and candidate ordering.
    std::vector<std::pair<double, size_t>> scoresAndCandidateIndices;
    std::vector<size_t> failed;
    std::vector<size_t> documentsExamined;

    // Compute score for each tree.  Record the best.
    for (size_t i = 0; i < statTrees.size(); ++i) {
        auto explainer = plan_explainer_factory::make(
            candidates[i].root, candidates[i].solution->_enumeratorExplainInfo);

        if (candidates[i].status.isOK()) {
            LOGV2_DEBUG(20956,
                        5,
                        "Scoring plan",
                        "planIndex"_attr = i,
                        "querySolution"_attr = redact(candidates[i].solution->toString()),
                        "stats"_attr = redact([&]() {
                            auto&& [stats, _] = explainer->getWinningPlanStats(
                                ExplainOptions::Verbosity::kExecStats);
                            return stats.jsonString(ExtendedRelaxedV2_0_0, true);
                        }()));
            LOGV2_DEBUG(20957,
                        2,
                        "Scoring query plan",
                        "planSummary"_attr = explainer->getPlanSummary(),
                        "planHitEOF"_attr = statTrees[i]->common.isEOF);

            double score = makePlanScorer()->calculateScore(statTrees[i].get(), cq);
            LOGV2_DEBUG(20958, 5, "Basic plan score", "score"_attr = score);
            if (statTrees[i]->common.isEOF) {
                LOGV2_DEBUG(20959, 5, "Adding EOF bonus to score", "eofBonus"_attr = eofBonus);
                score += 1;
            }

            candidates[i].solution->score = score;
            scoresAndCandidateIndices.push_back(std::make_pair(score, i));

            // Collect some information about documents examined for tie breaking later.
            PlanSummaryStats stats;
            explainer->getSummaryStats(&stats);
            documentsExamined.push_back(stats.totalDocsExamined);
        } else {
            failed.push_back(i);
            LOGV2_DEBUG(20960,
                        2,
                        "Not scoring a plan because the plan failed",
                        "planSummary"_attr = explainer->getPlanSummary());
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

    // Apply tie-breaking heuristics.
    if (internalQueryPlanTieBreakingWithIndexHeuristics.load()) {
        addTieBreakingHeuristicsBonuses(scoresAndCandidateIndices, candidates, documentsExamined);

        // Re-sort the candidates.
        std::stable_sort(scoresAndCandidateIndices.begin(),
                         scoresAndCandidateIndices.end(),
                         [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    }

    auto why = std::make_unique<PlanRankingDecision>();
    why->stats = StatsDetails{};

    // Update results in 'why'
    // Stats and scores in 'why' are sorted in descending order by score.
    why->failedCandidates = std::move(failed);
    for (size_t i = 0; i < scoresAndCandidateIndices.size(); ++i) {
        double score = scoresAndCandidateIndices[i].first;
        size_t candidateIndex = scoresAndCandidateIndices[i].second;

        why->stats.candidatePlanStats.push_back(std::move(statTrees[candidateIndex]));
        why->scores.push_back(score);
        why->candidateOrder.push_back(candidateIndex);
    }
    for (auto& i : why->failedCandidates) {
        why->stats.candidatePlanStats.push_back(std::move(statTrees[i]));
    }

    return StatusWith<std::unique_ptr<PlanRankingDecision>>(std::move(why));
}
}  // namespace mongo::plan_ranker

#undef MONGO_LOGV2_DEFAULT_COMPONENT
