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

        /**
         * Get the next result from the query.
         */
        virtual bool getNext(BSONObj* objOut) = 0;

        /**
         * Inform the runner that the provided DiskLoc is about to disappear (or change entirely).
         * The runner then takes any actions required to continue operating correctly, including
         * broadcasting the invalidation request to the PlanStage tree being run.
         *
         * Called from ClientCursor::aboutToDelete.
         */
        virtual void invalidate(const DiskLoc& dl) = 0;

        virtual void saveState() = 0;
        virtual void restoreState() = 0;

        /**
         * Return the query that the runner is running.
         */
        virtual const CanonicalQuery& getQuery() = 0;
    };

}  // namespace mongo
