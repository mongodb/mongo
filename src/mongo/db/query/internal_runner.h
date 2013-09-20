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
              : _ns(ns), _exec(new PlanExecutor(ws, root)), _policy(Runner::YIELD_MANUAL) { }

        virtual ~InternalRunner() {
            if (Runner::YIELD_AUTO == _policy) {
                ClientCursor::deregisterRunner(this);
            }
        }

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
            return _exec->getNext(objOut, dlOut);
        }

        virtual bool isEOF() { return _exec->isEOF(); }

        virtual void saveState() { _exec->saveState(); }

        virtual bool restoreState() { return _exec->restoreState(); }

        virtual const string& ns() { return _ns; }

        virtual void invalidate(const DiskLoc& dl) { _exec->invalidate(dl); }

        virtual void setYieldPolicy(Runner::YieldPolicy policy) {
            // No-op.
            if (_policy == policy) { return; }

            if (Runner::YIELD_AUTO == policy) {
                // Going from manual to auto.
                ClientCursor::registerRunner(this);
            }
            else {
                // Going from auto to manual.
                ClientCursor::deregisterRunner(this);
            }

            _policy = policy;
            _exec->setYieldPolicy(policy);
        }

        virtual void kill() { _exec->kill(); }

    private:
        string _ns;

        scoped_ptr<PlanExecutor> _exec;
        Runner::YieldPolicy _policy;
    };

}  // namespace mongo
