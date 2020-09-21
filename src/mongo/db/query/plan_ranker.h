/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <queue>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/util/container_size_helper.h"

namespace mongo::plan_ranker {
// The logging facility enforces the rule that logging should not be done in a header file. Since
// template classes and functions below must be defined in the header file and since they use the
// logging facility, we have to define the helper functions below to perform the actual logging
// operation from template code.
// Note that we pass std::function callback instead of string values to avoid spending time
// generating log output that may never actually be written to the logs, depending on the current
// log level.
namespace log_detail {
void logScoreFormula(std::function<std::string()> formula,
                     double score,
                     double baseScore,
                     double productivity,
                     double noFetchBonus,
                     double noSortBonus,
                     double noIxisectBonus,
                     double tieBreakers);
void logScoreBoost(double score);
void logScoringPlan(std::function<std::string()> solution,
                    std::function<std::string()> explain,
                    std::function<std::string()> planSummary,
                    size_t planIndex,
                    bool isEOF);
void logScore(double score);
void logEOFBonus(double eofBonus);
void logFailedPlan(std::function<std::string()> planSummary);
}  // namespace log_detail

/**
 * Assigns the stats tree a 'goodness' score. The higher the score, the better the plan. The exact
 * value isn't meaningful except for imposing a ranking.
 *
 * All specific plan scorers should inherit from this scorer and provide methods to produce the plan
 * productivity factor, and the number of plan "advances", representing the number of documents
 * returned by the PlanStage tree.
 */
template <typename PlanStageStatsType>
class PlanScorer {
public:
    PlanScorer() = default;
    virtual ~PlanScorer() = default;

    double calculateScore(const PlanStageStatsType* stats) const {
        // We start all scores at 1.  Our "no plan selected" score is 0 and we want all plans to
        // be greater than that.
        const double baseScore = 1;

        const auto productivity = calculateProductivity(stats);
        const auto advances = getNumberOfAdvances(stats);
        const double epsilon =
            std::min(1.0 / static_cast<double>(10 * (advances > 0 ? advances : 1)), 1e-4);


        // We prefer queries that don't require a fetch stage.
        double noFetchBonus = epsilon;
        if (hasStage(STAGE_FETCH, stats)) {
            noFetchBonus = 0;
        }

        // In the case of ties, prefer solutions without a blocking sort
        // to solutions with a blocking sort.
        double noSortBonus = epsilon;
        if (hasStage(STAGE_SORT_DEFAULT, stats) || hasStage(STAGE_SORT_SIMPLE, stats)) {
            noSortBonus = 0;
        }

        // In the case of ties, prefer single index solutions to ixisect. Index
        // intersection solutions are often slower than single-index solutions
        // because they require examining a superset of index keys that would be
        // examined by a single index scan.
        //
        // On the other hand, index intersection solutions examine the same
        // number or fewer of documents. In the case that index intersection
        // allows us to examine fewer documents, the penalty given to ixisect
        // can be made up via the no fetch bonus.
        double noIxisectBonus = epsilon;
        if (hasStage(STAGE_AND_HASH, stats) || hasStage(STAGE_AND_SORTED, stats)) {
            noIxisectBonus = 0;
        }

        const double tieBreakers = noFetchBonus + noSortBonus + noIxisectBonus;
        double score = baseScore + productivity + tieBreakers;

        log_detail::logScoreFormula([this, stats] { return getProductivityFormula(stats); },
                                    score,
                                    baseScore,
                                    productivity,
                                    noFetchBonus,
                                    noSortBonus,
                                    noIxisectBonus,
                                    tieBreakers);

        if (internalQueryForceIntersectionPlans.load()) {
            if (hasStage(STAGE_AND_HASH, stats) || hasStage(STAGE_AND_SORTED, stats)) {
                // The boost should be >2.001 to make absolutely sure the ixisect plan will win due
                // to the combination of 1) productivity, 2) eof bonus, and 3) no ixisect bonus.
                score += 3;
                log_detail::logScoreBoost(score);
            }
        }
        return score;
    }

protected:
    /**
     * Returns an abstract plan productivity value. Each implementation is free to define the
     * formula to calculate the productivity. The value must be withing the range: [0, 1].
     */
    virtual double calculateProductivity(const PlanStageStatsType* stats) const = 0;

    /**
     * Returns a string desribing a formula to calculte plan producivity. It can be used for the log
     * output, for example.
     */
    virtual std::string getProductivityFormula(const PlanStageStatsType* stats) const = 0;

    /**
     * Returns the number of advances from the root stage stats, which represents the number of
     * documents returned by the PlanStage tree.
     */
    virtual double getNumberOfAdvances(const PlanStageStatsType* stats) const = 0;

    /**
     * True, if the plan stage stats tree represents a plan stage of the given 'type'.
     */
    virtual bool hasStage(StageType type, const PlanStageStatsType* stats) const = 0;
};

/**
 * A container holding one to-be-scored plan and its associated/relevant data.
 * It takes the following template parameters:
 *    * PlanStageType - the type of plan stages in the execution tree.
 *    * ResultType - the type of data produced by the execution tree during the candidate plan
 *      execution.
 *    * Data - the type of any auxiliary data which is needed to run the execution tree.
 */
template <typename PlanStageType, typename ResultType, typename Data>
struct BaseCandidatePlan {
    // A query solution representing this candidate plan.
    std::unique_ptr<QuerySolution> solution;
    // A root stage of the PlanStage tree constructed from the 'solution'.
    PlanStageType root;
    // Any auxiliary data required to run the execution tree.
    Data data;
    // Indicates whether this candidate plan has completed the trial run early by achieving one
    // of the trial run metrics.
    bool exitedEarly{false};
    // Indicates that this candidate plan has failed in a recoverable fashion during the trial run.
    bool failed{false};
    // Any results produced during the plan's execution prior to scoring are retained here.
    std::queue<ResultType> results;
};

using CandidatePlan = BaseCandidatePlan<PlanStage*, WorkingSetID, WorkingSet*>;

/**
 * Information about why a plan was picked to be the best.  Data here is placed into the cache
 * and used to compare expected performance with actual.
 */
struct PlanRankingDecision {
    PlanRankingDecision() {}

    /**
     * Make a deep copy.
     */
    PlanRankingDecision* clone() const {
        PlanRankingDecision* decision = new PlanRankingDecision();
        stdx::visit(
            [&decision](auto&& planStats) {
                using StatsType = typename std::decay_t<decltype(planStats)>::value_type;
                std::vector<StatsType> copy;
                for (auto&& stats : planStats) {
                    invariant(stats);
                    copy.push_back(StatsType{stats->clone()});
                }
                decision->stats = std::move(copy);
            },
            stats);
        decision->scores = scores;
        decision->candidateOrder = candidateOrder;
        decision->failedCandidates = failedCandidates;
        return decision;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return  // Add size of each element in 'stats' vector.
            stdx::visit(
                [](auto&& stats) {
                    return container_size_helper::estimateObjectSizeInBytes(
                        stats, [](auto&& stat) { return stat->estimateObjectSizeInBytes(); }, true);
                },
                stats) +
            // Add size of each element in 'candidateOrder' vector.
            container_size_helper::estimateObjectSizeInBytes(candidateOrder) +
            // Add size of each element in 'failedCandidates' vector.
            container_size_helper::estimateObjectSizeInBytes(failedCandidates) +
            // Add size of each element in 'scores' vector.
            container_size_helper::estimateObjectSizeInBytes(scores) +
            // Add size of the object.
            sizeof(*this);
    }

    template <typename PlanStageStatsType>
    const std::vector<std::unique_ptr<PlanStageStatsType>>& getStats() const {
        return stdx::get<std::vector<std::unique_ptr<PlanStageStatsType>>>(stats);
    }

    template <typename PlanStageStatsType>
    std::vector<std::unique_ptr<PlanStageStatsType>>& getStats() {
        return stdx::get<std::vector<std::unique_ptr<PlanStageStatsType>>>(stats);
    }

    // Stats of all plans sorted in descending order by score.
    stdx::variant<std::vector<std::unique_ptr<PlanStageStats>>,
                  std::vector<std::unique_ptr<mongo::sbe::PlanStageStats>>>
        stats;

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

    // Whether two plans tied for the win.
    //
    // Reading this flag is the only reliable way for callers to determine if there was a tie,
    // because the scores kept inside the PlanRankingDecision do not incorporate the EOF bonus.
    bool tieForBest = false;
};

/**
 * A factory function to create a plan scorer for a plan stage stats tree.
 */
std::unique_ptr<PlanScorer<PlanStageStats>> makePlanScorer();
}  // namespace mongo::plan_ranker

// Forward declaration.
namespace mongo::sbe::plan_ranker {
std::unique_ptr<mongo::plan_ranker::PlanScorer<PlanStageStats>> makePlanScorer(
    const QuerySolution* solution);
}  // namespace mongo::sbe::plan_ranker

namespace mongo::plan_ranker {
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
        statTrees.push_back(candidates[i].root->getStats());
    }

    // Holds (score, candidateIndex).
    // Used to derive scores and candidate ordering.
    std::vector<std::pair<double, size_t>> scoresAndCandidateIndices;
    std::vector<size_t> failed;

    // Compute score for each tree.  Record the best.
    for (size_t i = 0; i < statTrees.size(); ++i) {
        if (!candidates[i].failed) {
            log_detail::logScoringPlan(
                [& candidate = candidates[i]]() { return candidate.solution->toString(); },
                [root = &*candidates[i].root, solution = candidates[i].solution.get()]() {
                    auto explainer = plan_explainer_factory::makePlanExplainer(root, solution);
                    auto&& [stats, _] =
                        explainer->getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                    return stats.jsonString(ExtendedRelaxedV2_0_0, true);
                },
                [root = &*candidates[i].root, solution = candidates[i].solution.get()]() {
                    auto explainer = plan_explainer_factory::makePlanExplainer(root, solution);
                    return explainer->getPlanSummary();
                },
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
            log_detail::logFailedPlan(
                [root = &*candidates[i].root, solution = candidates[i].solution.get()] {
                    auto explainer = plan_explainer_factory::makePlanExplainer(root, solution);
                    return explainer->getPlanSummary();
                });
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
    why->stats = std::vector<std::unique_ptr<PlanStageStatsType>>{};

    // Determine whether plans tied for the win.
    if (scoresAndCandidateIndices.size() > 1U) {
        double bestScore = scoresAndCandidateIndices[0].first;
        double runnerUpScore = scoresAndCandidateIndices[1].first;
        const double epsilon = 1e-10;
        why->tieForBest = std::abs(bestScore - runnerUpScore) < epsilon;
    }

    // Update results in 'why'
    // Stats and scores in 'why' are sorted in descending order by score.
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

        why->getStats<PlanStageStatsType>().push_back(std::move(statTrees[candidateIndex]));
        why->scores.push_back(score);
        why->candidateOrder.push_back(candidateIndex);
    }
    for (auto& i : why->failedCandidates) {
        why->getStats<PlanStageStatsType>().push_back(std::move(statTrees[i]));
    }

    return StatusWith<std::unique_ptr<PlanRankingDecision>>(std::move(why));
}
}  // namespace mongo::plan_ranker
