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

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/runner.h"
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
              _exec(new PlanExecutor(ws, root)), _updatedCache(false) { }

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
            Runner::RunnerState state = _exec->getNext(objOut, dlOut);

            if (Runner::RUNNER_EOF == state && !_updatedCache) {
                updateCache();
            }

            return state;
        }

        virtual bool isEOF() { return _exec->isEOF(); }

        virtual void saveState() { _exec->saveState(); }

        virtual bool restoreState() { return _exec->restoreState(); }

        virtual void invalidate(const DiskLoc& dl) { _exec->invalidate(dl); }

        virtual void setYieldPolicy(Runner::YieldPolicy policy) {
            _exec->setYieldPolicy(policy);
        }

        virtual const string& ns() { return _canonicalQuery->getParsed().ns(); }

        virtual void kill() { _exec->kill(); }

    private:
        void updateCache() {
            _updatedCache = true;

            // We're done.  Update the cache.
            PlanCache* cache = PlanCache::get(_canonicalQuery->ns());

            // TODO: Is this an error?
            if (NULL == cache) { return; }

            // TODO: How do we decide this?
            bool shouldRemovePlan = false;

            if (shouldRemovePlan) {
                if (!cache->remove(*_canonicalQuery, *_cachedQuery->solution)) {
                    warning() << "Cached plan runner couldn't remove plan from cache.  Maybe"
                        " somebody else did already?";
                    return;
                }
            }

            // We're done running.  Update cache.
            auto_ptr<CachedSolutionFeedback> feedback(new CachedSolutionFeedback());
            feedback->stats = _exec->getStats();
            cache->feedback(*_canonicalQuery, *_cachedQuery->solution, feedback.release());
        }

        scoped_ptr<CanonicalQuery> _canonicalQuery;
        scoped_ptr<CachedSolution> _cachedQuery;
        scoped_ptr<PlanExecutor> _exec;

        // Have we updated the cache with our plan stats yet?
        bool _updatedCache;
    };

}  // namespace mongo
