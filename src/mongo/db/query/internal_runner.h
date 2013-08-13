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

#include "mongo/db/clientcursor.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo {

    /**
     * This is a runner that was requested by an internal client of the query system, as opposed to
     * runners that are built in response to a query entering the system. It is only used by
     * internal clients of the query systems (e.g., chunk migration, index building, commands that
     * traverse data such as md5, ... )
     *
     * The salient feature of this Runner is that it does not interact with the cache at all.
     */
    class InternalRunner : public Runner {
    public:
        /**
         * Takes ownership of all arguments.
         */
        InternalRunner(const string& ns, PlanStage* root, WorkingSet* ws)
              : _ns(ns), _exec(new PlanExecutor(ws, root)) {
            _canonicalQuery.reset(CanonicalQuery::getInternalQuery());
        }

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
            return _exec->getNext(objOut, dlOut);
        }

        virtual void saveState() { _exec->saveState(); }

        virtual bool restoreState() { return _exec->restoreState(); }

        virtual const string& ns() { return _ns; }

        virtual void invalidate(const DiskLoc& dl) { _exec->invalidate(dl); }

        virtual void setYieldPolicy(Runner::YieldPolicy policy) {
            _exec->setYieldPolicy(policy);
        }

        // TODO: Remove this from the runner spec, only used in creating a ClientCursor.
        virtual const CanonicalQuery& getQuery() { 
            verify(0);
            return *_canonicalQuery;
        }

        virtual void kill() { _exec->kill(); }

    private:
        string _ns;

        // This is a dummy query.
        scoped_ptr<CanonicalQuery> _canonicalQuery;
        scoped_ptr<PlanExecutor> _exec;
    };

}  // namespace mongo
