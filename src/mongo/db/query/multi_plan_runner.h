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

#pragma once

#include <boost/scoped_ptr.hpp>
#include <queue>
#include <vector>

#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/simple_plan_runner.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    using std::queue;
    using std::size_t;
    using std::vector;

    /**
     * Runs several plans in parallel and picks the best one.  Caches the selection for future use.
     */
    class MultiPlanRunner : public Runner {
    public:
        /**
         * Takes ownership of query.
         */
        MultiPlanRunner(CanonicalQuery* query);
        virtual ~MultiPlanRunner();

        /**
         * Takes ownership of all arguments
         */
        void addPlan(QuerySolution* solution, PlanStage* root, WorkingSet* ws);

        /**
         * Get the next result.  Yielding is handled internally.  If a best plan is not picked when
         * this is called, we call pickBestPlan() internally.
         */
        bool getNext(BSONObj* objOut);

        /**
         * Runs all plans added by addPlan, ranks them, and picks a best.  Deletes all loser plans.
         * All further calls to getNext(...) will return results from the best plan.
         *
         * Returns true if a best plan was picked, false if there was an error.
         *
         * If out is not-NULL, set *out to the index of the picked plan.
         */
        bool pickBestPlan(size_t* out);

        virtual void yield();
        virtual void unYield();
        virtual void invalidate(const DiskLoc& dl);

    private:
        /**
         * Have all our candidate plans do something.
         */
        bool workAllPlans();
        void yieldAllPlans();
        void unyieldAllPlans();

        // Did some plan fail?  Just give up if so.
        bool _failure;

        // The winner...
        scoped_ptr<SimplePlanRunner> _bestPlanRunner;
        // ...and any results it produced while working toward winning.
        std::queue<WorkingSetID> _alreadyProduced;

        // Candidate plans.
        vector<CandidatePlan> _candidates;

        // The query that we're trying to figure out the best solution to.
        scoped_ptr<CanonicalQuery> _query;
    };

}  // namespace mongo
