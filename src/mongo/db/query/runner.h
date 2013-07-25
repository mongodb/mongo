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

namespace mongo {

    /**
     * A runner runs a query.  All yielding, fetching, and other query details are taken care of by
     * the runner.
     *
     * TODO: Do we want to expand the interface to allow yielding?  IE, if update is running a query
     * and updating at the same time?
     */
    class Runner {
    public:
        /**
         * Get the next result from the query.
         */
        // TODO: This is inefficient and should probably append to some message buffer or similar.
        virtual bool getNext(BSONObj* objOut) = 0;

        virtual ~Runner() { }
    };

}  // namespace mongo
