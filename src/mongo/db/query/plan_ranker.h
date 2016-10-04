/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <list>
#include <memory>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

struct CandidatePlan;
struct PlanRankingDecision;

/**
 * Ranks 2 or more plans.
 */
class PlanRanker {
public:
    /**
     * Returns index in 'candidates' of which plan is best.
     * Populates 'why' with information relevant to how each plan fared in the ranking process.
     * Caller owns pointers in 'why'.
     * 'candidateOrder' holds indices into candidates ordered by score (winner in first element).
     */
    static size_t pickBestPlan(const std::vector<CandidatePlan>& candidates,
                               PlanRankingDecision* why);

    /**
     * Assign the stats tree a 'goodness' score. The higher the score, the better
     * the plan. The exact value isn't meaningful except for imposing a ranking.
     */
    static double scoreTree(const PlanStageStats* stats);
};

/**
 * A container holding one to-be-ranked plan and its associated/relevant data.
 * Does not own any of its pointers.
 */
struct CandidatePlan {
    CandidatePlan(QuerySolution* s, PlanStage* r, WorkingSet* w)
        : solution(s), root(r), ws(w), failed(false) {}

    std::unique_ptr<QuerySolution> solution;
    PlanStage* root;  // Not owned here.
    WorkingSet* ws;   // Not owned here.

    // Any results produced during the plan's execution prior to ranking are retained here.
    std::list<WorkingSetID> results;

    bool failed;
};

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
        for (size_t i = 0; i < stats.size(); ++i) {
            PlanStageStats* s = stats.vector()[i];
            invariant(s);
            decision->stats.mutableVector().push_back(s->clone());
        }
        decision->scores = scores;
        decision->candidateOrder = candidateOrder;
        return decision;
    }

    // Stats of all plans sorted in descending order by score.
    // Owned by us.
    OwnedPointerVector<PlanStageStats> stats;

    // The "goodness" score corresponding to 'stats'.
    // Sorted in descending order.
    std::vector<double> scores;

    // Ordering of original plans in descending of score.
    // Filled in by PlanRanker::pickBestPlan(candidates, ...)
    // so that candidates[candidateOrder[0]] refers to the best plan
    // with corresponding cores[0] and stats[0]. Runner-up would be
    // candidates[candidateOrder[1]] followed by
    // candidates[candidateOrder[2]], ...
    std::vector<size_t> candidateOrder;

    // Whether two plans tied for the win.
    //
    // Reading this flag is the only reliable way for callers to determine if there was a tie,
    // because the scores kept inside the PlanRankingDecision do not incorporate the EOF bonus.
    bool tieForBest = false;
};

}  // namespace mongo
