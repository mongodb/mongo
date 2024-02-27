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

#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/platform/atomic_word.h"
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
void logTieBreaking(double score,
                    double docsFetchedBonus,
                    double indexPrefixBonus,
                    bool isPlanTied);
}  // namespace log_detail

// Constant used for tie breakers.
const double kBonusEpsilon = 1e-4;

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
            std::min(1.0 / static_cast<double>(10 * (advances > 0 ? advances : 1)), kBonusEpsilon);


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
    // If the candidate plan has failed in a recoverable fashion during the trial run, contains a
    // non-OK status.
    Status status{Status::OK()};
    // Indicates whether this candidate plan was retrieved from the cache. During explain, we do not
    // fetch plans from the cache, so it may be the case that a plan matches a cache entry even when
    // it is not fetched from the cache.
    bool fromPlanCache{false};
    // Any results produced during the plan's execution prior to scoring are retained here.
    std::deque<ResultType> results;
    // This is used to track the original plan with clean PlanStage tree and the auxiliary data.
    // The 'root' and 'data' in this struct could be used to execute trials in multi-planner before
    // caching the winning plan, which requires necessary values bound to 'data'. These values
    // should not be stored in the plan cache.
    boost::optional<std::pair<PlanStageType, Data>> clonedPlan;
};

using CandidatePlan = BaseCandidatePlan<PlanStage*, WorkingSetID, WorkingSet*>;

/**
 * Apply index prefix heuristic (see comment to 'getIndexBoundsScore' function in the cpp file) for
 * the given list of solutions, if the solutions are compatible (have the same plan shape), the
 * vector of winner indexes are returned, otherwise an empty vector is returned.
 */
std::vector<size_t> applyIndexPrefixHeuristic(const std::vector<const QuerySolution*>& solutions);
}  // namespace mongo::plan_ranker
