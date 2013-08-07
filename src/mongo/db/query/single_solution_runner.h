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

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/simple_plan_runner.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo {

    /**
     * SingleSolutionRunner runs a plan that was the only possible solution to a query.  It exists
     * only to dump stats into the cache after running.
     */
    class SingleSolutionRunner : public Runner {
    public:
        /**
         * Takes ownership of both arguments.
         */
        SingleSolutionRunner(CanonicalQuery* canonicalQuery, QuerySolution* soln,
                             PlanStage* root, WorkingSet* ws)
            : _canonicalQuery(canonicalQuery), _solution(soln),
              _runner(new SimplePlanRunner(ws, root)) { }

        bool getNext(BSONObj* objOut) {
            // Use the underlying runner until it's exhausted.
            if (_runner->getNext(objOut)) {
                return true;
            }

            // TODO: I'm not convinced we want to cache this.  What if it's a collscan solution and
            // the user adds an index later?  We don't want to reach for this.

            // We're done.  Update the cache.
            //PlanCache* cache = PlanCache::get(_canonicalQuery->ns());
            // TODO: is this a verify?
            //if (NULL == cache) { return false; }
            // We're done running.  Update cache.
            //auto_ptr<PlanRankingDecision> why(new PlanRankingDecision());
            //why->onlyOneSolution = true;
            //cache->add(canonicalQuery.release(), solutions[0], why.release());
            return false;
        }

        virtual void saveState() { _runner->saveState(); }
        virtual void restoreState() { _runner->restoreState(); }
        virtual void invalidate(const DiskLoc& dl) { _runner->invalidate(dl); }

        virtual const CanonicalQuery& getQuery() {
            return *_canonicalQuery;
        }

    private:
        scoped_ptr<CanonicalQuery> _canonicalQuery;
        scoped_ptr<QuerySolution> _solution;
        scoped_ptr<SimplePlanRunner> _runner;
    };

}  // namespace mongo

