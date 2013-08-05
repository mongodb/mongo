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
     * CachedPlanRunner runs a plan retrieved from the cache.
     *
     * Cached plans are bundled with information describing why the plan is in the cache.
     *
     * If we run a plan from the cache and behavior wildly deviates from expected behavior, we may
     * remove the plan from the cache.  See plan_cache.h.
     */
    class CachedPlanRunner : public Runner {
    public:
        /**
         * Takes ownership of both arguments.
         */
        CachedPlanRunner(CanonicalQuery* canonicalQuery, CachedSolution* cached,
                         PlanStage* root, WorkingSet* ws)
            : _canonicalQuery(canonicalQuery), _cachedQuery(cached),
              _runner(new SimplePlanRunner(ws, root)) { }

        bool getNext(BSONObj* objOut) {
            // Use the underlying runner until it's exhausted.
            if (_runner->getNext(objOut)) {
                return true;
            }

            // We're done.  Update the cache.
            PlanCache* cache = PlanCache::get(_canonicalQuery->ns());

            // TODO: is this a verify?
            if (NULL == cache) { return false; }

            // TODO: How do we decide this?
            bool shouldRemovePlan = false;

            if (shouldRemovePlan) {
                if (!cache->remove(*_canonicalQuery, *_cachedQuery->solution)) {
                    warning() << "Cached plan runner couldn't remove plan from cache.  Maybe"
                                 " somebody else did already?";
                }
                return false;
            }

            // We're done running.  Update cache.
            auto_ptr<CachedSolutionFeedback> feedback(new CachedSolutionFeedback());
            feedback->stats = _runner->getStats();
            cache->feedback(*_canonicalQuery, *_cachedQuery->solution, feedback.release());
            return false;
        }

        /**
         * TODO: Explicit yielding is deprecated pending a ClientCursor rewrite.
         */
        virtual void yield() { _runner->yield(); }
        virtual void unYield() { _runner->unYield(); }

        virtual void invalidate(const DiskLoc& dl) {
            _runner->invalidate(dl);
        }

    private:
        scoped_ptr<CanonicalQuery> _canonicalQuery;
        scoped_ptr<CachedSolution> _cachedQuery;
        scoped_ptr<SimplePlanRunner> _runner;
    };

}  // namespace mongo
