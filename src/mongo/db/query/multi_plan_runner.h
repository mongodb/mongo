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

#include <boost/scoped_ptr.hpp>
#include <list>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/plan_ranker.h" // for CandidatePlan
#include "mongo/db/query/runner.h"
#include "mongo/db/query/runner_yield_policy.h"

namespace mongo {

    class BSONObj;
    class CanonicalQuery;
    class DiskLoc;
    class PlanExecutor;
    class PlanStage;
    struct QuerySolution;
    class TypeExplain;
    struct PlanInfo;
    class WorkingSet;

    /**
     * Runs several plans in parallel and picks the best one.  Caches the selection for future use.
     */
    class MultiPlanRunner : public Runner {
    public:
        /**
         * Takes ownership of query.
         */
        MultiPlanRunner(const Collection* collection, CanonicalQuery* query);
        virtual ~MultiPlanRunner();

        /**
         * Takes ownership of all arguments
         */
        void addPlan(QuerySolution* solution, PlanStage* root, WorkingSet* ws);

        /**
         * Get the next result.  Yielding is handled internally.  If a best plan is not picked when
         * this is called, we call pickBestPlan() internally.
         */
        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        virtual bool isEOF();

        /**
         * Runs all plans added by addPlan, ranks them, and picks a best.  Deletes all loser plans.
         * All further calls to getNext(...) will return results from the best plan.
         *
         * Returns true if a best plan was picked, false if there was an error.
         *
         * If out is not-NULL, set *out to the index of the picked plan.
         */
        bool pickBestPlan(size_t* out);

        virtual void saveState();
        virtual bool restoreState();
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual void setYieldPolicy(Runner::YieldPolicy policy);

        virtual const std::string& ns();

        virtual void kill();

        virtual const Collection* collection() { return _collection; }

        /**
         * Returns OK, allocating and filling in '*explain' and '*planInfo' with details of
         * the "winner" plan. Caller takes ownership of '*explain' and '*planInfo'. Otherwise,
         * return a status describing the error.
         *
         * TOOD: fill in the explain of all candidate plans
         */
        virtual Status getInfo(TypeExplain** explain,
                               PlanInfo** planInfo) const;

    private:
        /**
         * Have all our candidate plans do something.
         */
        bool workAllPlans();
        void allPlansSaveState();
        void allPlansRestoreState();

        const Collection* _collection;

        // Were we killed by an invalidate?
        bool _killed;

        // Did all plans fail while we were running them?  Note that one plan can fail
        // during normal execution of the plan competition.  Here is an example:
        //
        // Plan 1: collection scan with sort.  Sort runs out of memory.
        // Plan 2: ixscan that provides sort.  Won't run out of memory.
        //
        // We want to choose plan 2 even if plan 1 fails.
        bool _failure;

        // If everything fails during the plan competition, we can't pick one.
        size_t _failureCount;

        // We need to cache this so that when we switch from running our candidates to using a
        // PlanExecutor, we can set the right yielding policy on it.
        Runner::YieldPolicy _policy;

        // The winner of the plan competition...
        boost::scoped_ptr<PlanExecutor> _bestPlan;

        // ...and any results it produced while working toward winning.
        std::list<WorkingSetID> _alreadyProduced;

        // ...and the solution, for caching.
        boost::scoped_ptr<QuerySolution> _bestSolution;

        // Candidate plans.
        std::vector<CandidatePlan> _candidates;

        // Candidate plans' stats. Owned here.
        std::vector<PlanStageStats*> _candidateStats;

        // Yielding policy we use when we're running candidates.
        boost::scoped_ptr<RunnerYieldPolicy> _yieldPolicy;

        // The query that we're trying to figure out the best solution to.
        boost::scoped_ptr<CanonicalQuery> _query;

        //
        // Backup plan for sort
        //

        QuerySolution* _backupSolution;
        PlanExecutor* _backupPlan;
        std::list<WorkingSetID> _backupAlreadyProduced;
    };

}  // namespace mongo
