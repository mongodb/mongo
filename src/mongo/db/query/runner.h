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

namespace mongo {

    /**
     * A runner runs a query.
     */
    class Runner {
    public:
        virtual ~Runner() { }

        enum RunnerState {
            // We successfully populated the out parameter.
            RUNNER_ADVANCED,

            // We're EOF.  We won't return any more results (edge case exception: capped+tailable).
            RUNNER_EOF,

            // We were killed or had an error.
            RUNNER_DEAD,
        };

        /**
         * Get the next result from the query.
         */
        virtual RunnerState getNext(BSONObj* objOut) = 0;

        /**
         * Inform the runner that the provided DiskLoc is about to disappear (or change entirely).
         * The runner then takes any actions required to continue operating correctly, including
         * broadcasting the invalidation request to the PlanStage tree being run.
         *
         * Called from ClientCursor::aboutToDelete.
         */
        virtual void invalidate(const DiskLoc& dl) = 0;

        /**
         * Save any state required to yield.
         */
        virtual void saveState() = 0;

        /**
         * Restore saved state, possibly after a yield.
         */
        virtual void restoreState() = 0;

        /**
         * Return the query that the runner is running.
         */
        virtual const CanonicalQuery& getQuery() = 0;

        /**
         * Mark the Runner as no longer valid.  Can happen when a runner yields and the underlying
         * database is dropped/indexes removed/etc.
         */
        virtual void kill() = 0;

        /**
         * Force the runner to yield.  Gives up any locks it had before.  Holds locks again when the
         * runner returns from the yield.  Returns true if it's OK to continue using the runner,
         * false if the runner was killed during a yield.
         */
        virtual bool forceYield() = 0;
    };

}  // namespace mongo
