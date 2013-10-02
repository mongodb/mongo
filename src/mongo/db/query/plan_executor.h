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

#include "mongo/base/status.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/runner_yield_policy.h"

namespace mongo {

    class BSONObj;
    class DiskLoc;
    class PlanStage;
    struct PlanStageStats;
    class WorkingSet;

    /**
     * A PlanExecutor is the abstraction that knows how to crank a tree of stages into execution.
     * The executor is usually part of a larger abstraction that is interacting with the cache
     * and/or the query optimizer.
     *
     * Executes a plan.  Used by a runner.  Calls work() on a plan until a result is produced.
     * Stops when the plan is EOF or if the plan errors.
     */
    class PlanExecutor {
    public:
        PlanExecutor(WorkingSet* ws, PlanStage* rt);
        ~PlanExecutor();

        //
        // Accessors
        //

        /** TODO document me */
        WorkingSet* getWorkingSet();

        /** This is OK even if we were killed */
        PlanStageStats* getStats() const;

        //
        // Methods that just pass down to the PlanStage tree.
        //

        /** TODO document me */
        void saveState();

        /** TODO document me */
        bool restoreState();

        /** TODO document me */
        void invalidate(const DiskLoc& dl);

        //
        // Running Support
        //

        /** TODO document me */
        void setYieldPolicy(Runner::YieldPolicy policy);

        /** TODO document me */
        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        /** TOOD document me */
        bool isEOF();

        /**
         * During the yield, the database we're operating over or any collection we're relying on
         * may be dropped.  When this happens all cursors and runners on that database and
         * collection are killed or deleted in some fashion. (This is how the _killed gets set.)
         */
        void kill();

    private:
        boost::scoped_ptr<WorkingSet> _workingSet;
        boost::scoped_ptr<PlanStage> _root;
        boost::scoped_ptr<RunnerYieldPolicy> _yieldPolicy;

        // Did somebody drop an index we care about or the namespace we're looking at?  If so,
        // we'll be killed.
        bool _killed;
    };

}  // namespace mongo
