/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#include <vector>

#include "mongo/db/query/plan_ranker.h"

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    // static 
    size_t PlanRanker::pickBestPlan(const vector<CandidatePlan>& candidates,
                                    PlanRankingDecision* why) {
        // Each plan will have a stat tree.
        vector<PlanStageStats*> statTrees;

        // Get stat trees from each plan.
        for (size_t i = 0; i < candidates.size(); ++i) {
            statTrees.push_back(candidates[i].root->getStats());
        }

        // Compute score for each tree.  Record the best.
        double maxScore = 0;
        size_t bestChild = numeric_limits<size_t>::max();
        for (size_t i = 0; i < statTrees.size(); ++i) {
            double score = scoreTree(*statTrees[i]);
            if (score > maxScore) {
                maxScore = score;
                bestChild = i;
            }
        }

        // Make sure we got something.
        verify(numeric_limits<size_t>::max() != bestChild);

        if (NULL != why) {
            // Record the stats of the winner.
            why->statsOfWinner = statTrees[bestChild];
        }

        // Clean up stats of losers.
        for (size_t i = 0; i < statTrees.size(); ++i) {
            // If why is null we're not saving the bestChild's stats and we can delete it.
            if (i != bestChild || NULL == why) {
                delete statTrees[i];
            }
        }

        return bestChild;
    }

    // static 
    double PlanRanker::scoreTree(const PlanStageStats& stats) {
        // EOF?  You win!
        if (stats.common.isEOF) {
            return std::numeric_limits<double>::max();
        }
        else {
            // This is a placeholder for better ranking logic.
            return static_cast<double>(stats.common.advanced)
                   / static_cast<double>(stats.common.works);
        }
    }

}  // namespace mongo
