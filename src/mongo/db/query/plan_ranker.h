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

#include <queue>
#include <vector>

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
         * If 'why' is not NULL, populates it with information relevant to why that plan was picked.
         */
        static size_t pickBestPlan(const vector<CandidatePlan>& candidates,
                                   PlanRankingDecision* why);
    private:
        /**
         * Assign the stats tree a 'goodness' score.  Used internally.
         */
        static double scoreTree(const PlanStageStats& stats);
    };

    /**
     * A container holding one to-be-ranked plan and its associated/relevant data.
     * Does not own any of its pointers.
     */
    struct CandidatePlan {
        CandidatePlan(QuerySolution* s, PlanStage* r, WorkingSet* w)
            : solution(s), root(r), ws(w) { }

        QuerySolution* solution;
        PlanStage* root;
        WorkingSet* ws;

        // Any results produced during the plan's execution prior to ranking are retained here.
        std::queue<WorkingSetID> results;
    };

    /**
     * Information about why a plan was picked to be the best.  Data here is placed into the cache
     * and used by the CachedPlanRunner to compare expected performance with actual.
     */
    struct PlanRankingDecision {
        PlanRankingDecision() : statsOfWinner(NULL), onlyOneSolution(false) { }

        // Owned by us.
        PlanStageStats* statsOfWinner;

        bool onlyOneSolution;

        // TODO: We can place anything we want here.  What's useful to the cache?  What's useful to
        // planning and optimization?
    };

}  // namespace mongo
