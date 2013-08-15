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

#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * An interface for projecting (modifying) a WSM.
     * TODO: Add unit test.
     */
    class QueryProjection {
    public:
        virtual ~QueryProjection() { }

        /**
         * Compute the projection over the WSM.  Place the output in 'out'.
         */
        virtual Status project(const WorkingSetMember& wsm, BSONObj* out) = 0;

        /**
         * This projection handles the inclusion/exclusion syntax of the .find() command.
         * For details, see http://docs.mongodb.org/manual/reference/method/db.collection.find/
         */
        static Status newInclusionExclusion(const BSONObj& inclExcl, QueryProjection** out);
    };

}  // namespace mongo
