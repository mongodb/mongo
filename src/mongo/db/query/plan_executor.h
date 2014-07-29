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
#include "mongo/db/invalidation_type.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    class BSONObj;
    class Collection;
    class DiskLoc;
    class PlanStage;
    class PlanExecutor;
    struct PlanStageStats;
    class WorkingSet;

    /**
     * RAII approach to ensuring that plan executors are deregistered.
     *
     * While retrieving the first batch of results, newRunQuery manually registers the executor with
     * ClientCursor.  Certain query execution paths, namely $where, can throw an exception.  If we
     * fail to deregister the executor, we will call invalidate/kill on the
     * still-registered-yet-deleted executor.
     *
     * For any subsequent calls to getMore, the executor is already registered with ClientCursor
     * by virtue of being cached, so this exception-proofing is not required.
     */
    struct ScopedExecutorRegistration {
        ScopedExecutorRegistration(PlanExecutor* exec);
        ~ScopedExecutorRegistration();

        PlanExecutor* const _exec;
    };

    /**
     * A PlanExecutor is the abstraction that knows how to crank a tree of stages into execution.
     * The executor is usually part of a larger abstraction that is interacting with the cache
     * and/or the query optimizer.
     *
     * Executes a plan. Calls work() on a plan until a result is produced. Stops when the plan is
     * EOF or if the plan errors.
     */
    class PlanExecutor {
    public:

        enum ExecState {
            // We successfully populated the out parameter.
            ADVANCED,

            // We're EOF.  We won't return any more results (edge case exception: capped+tailable).
            IS_EOF,

            // We were killed or had an error.
            DEAD,

            // getNext was asked for data it cannot provide, or the underlying PlanStage had an
            // unrecoverable error.
            // If the underlying PlanStage has any information on the error, it will be available in
            // the objOut parameter. Call WorkingSetCommon::toStatusString() to retrieve the error
            // details from the output BSON object.
            EXEC_ERROR,
        };

        static std::string statestr(ExecState s) {
            if (PlanExecutor::ADVANCED == s) {
                return "ADVANCED";
            }
            else if (PlanExecutor::IS_EOF == s) {
                return "IS_EOF";
            }
            else if (PlanExecutor::DEAD == s) {
                return "DEAD";
            }
            else {
                verify(PlanExecutor::EXEC_ERROR == s);
                return "EXEC_ERROR";
            }
        }

        //
        // Constructors / destructor.
        //

        /**
         * Used when there is no canonical query and no query solution.
         *
         * Right now this is only for idhack updates which neither canonicalize
         * nor go through normal planning.
         */
        PlanExecutor(WorkingSet* ws, PlanStage* rt, const Collection* collection);

        /**
         * Used when we have a NULL collection and no canonical query. In this case,
         * we need to explicitly pass a namespace to the plan executor.
         */
        PlanExecutor(WorkingSet* ws, PlanStage* rt, std::string ns);

        /**
         * Used when there is a canonical query but no query solution (e.g. idhack
         * queries, queries against a NULL collection, queries using the subplan stage).
         */
        PlanExecutor(WorkingSet* ws, PlanStage* rt, CanonicalQuery* cq,
                     const Collection* collection);

        /**
         * The constructor for the normal case, when you have both a canonical query
         * and a query solution.
         */
        PlanExecutor(WorkingSet* ws, PlanStage* rt, QuerySolution* qs,
                     CanonicalQuery* cq, const Collection* collection);

        ~PlanExecutor();

        //
        // Accessors
        //

        /**
         * Get the working set used by this executor, withour transferring ownership.
         */
        WorkingSet* getWorkingSet() const;

        /**
         * Get the stage tree wrapped by this executor, without transferring ownership.
         */
        PlanStage* getRootStage() const;

        /**
         * Get the query that this executor is executing, without transferring ownership.
         */
        CanonicalQuery* getCanonicalQuery() const;

        /**
         * The collection in which this executor is working.
         */
        const Collection* collection() const;

        /**
         * Generates a tree of stats objects with a separate lifetime from the execution
         * stage tree wrapped by this PlanExecutor. The caller owns the returned pointer.
         *
         * This is OK even if we were killed.
         */
        PlanStageStats* getStats() const;

        //
        // Methods that just pass down to the PlanStage tree.
        //

        /** TODO document me */
        void saveState();

        /** TODO document me */
        bool restoreState(OperationContext* opCtx);

        /** TODO document me */
        void invalidate(const DiskLoc& dl, InvalidationType type);

        //
        // Running Support
        //

        /** TODO document me */
        ExecState getNext(BSONObj* objOut, DiskLoc* dlOut);

        /** TOOD document me */
        bool isEOF();

        /**
         * Register this plan executor with the collection cursor cache so that it
         * receives event notifications.
         *
         * Deregistration happens automatically when this plan executor is destroyed.
         *
         * Used just for internal plans:
         *  -- InternalPlanner::collectionScan(...) (see internal_plans.h)
         *  -- InternalPlanner::indexScan(...) (see internal_plans.h)
         *  -- getOplogStartHack(...) (see new_find.cpp)
         *  -- storeCurrentLocs(...) (see d_migrate.cpp)
         *
         * TODO: we probably don't need this for 2.8.
         */
        void registerExecInternalPlan();

        /**
         * During the yield, the database we're operating over or any collection we're relying on
         * may be dropped.  When this happens all cursors and plan executors on that database and
         * collection are killed or deleted in some fashion. (This is how the _killed gets set.)
         */
        void kill();

        /**
         * Execute the plan to completion, throwing out the results.
         *
         * Used by explain.
         */
        Status executePlan();

        /**
         * Return the NS that the query is running over.
         */
        const std::string& ns();

    private:
        /**
         * Initialize the namespace using either the canonical query or the collection.
         */
        void initNs();

        // Collection over which this plan executor runs. Used to resolve record ids retrieved by
        // the plan stages. The collection must not be destroyed while there are active plans.
        const Collection* _collection;

        boost::scoped_ptr<CanonicalQuery> _cq;
        boost::scoped_ptr<WorkingSet> _workingSet;
        boost::scoped_ptr<QuerySolution> _qs;
        std::auto_ptr<PlanStage> _root;

        // Deregisters this executor when it is destroyed.
        boost::scoped_ptr<ScopedExecutorRegistration> _safety;

        std::string _ns;

        // Did somebody drop an index we care about or the namespace we're looking at?  If so,
        // we'll be killed.
        bool _killed;
    };

}  // namespace mongo
