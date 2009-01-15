/* queryoptimizer.h */

/**
*    Copyright (C) 2008 10gen Inc.
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

    class QueryPlan {
    public:
        QueryPlan() {
            scanAndOrderRequired = false;
            simpleKeyMatch = false;
        }

        auto_ptr<Cursor> cursor;

        /* ScanAndOrder processing will be required if true */
        bool scanAndOrderRequired;

        /* When true, the index we are using has keys such that it can completely resolve the
           query expression to match by itself without ever checking the main object.
           */
        bool simpleKeyMatch;
    };

    /* We put these objects inside the Database objects: that way later if we want to do
       stats, it's in the right place.
    */
    class QueryOptimizer {
    public:
        QueryPlan getPlan(
            const char *ns,
            BSONObj* query,
            BSONObj* order = 0,
            BSONObj* hint = 0);
    };

} // namespace mongo
