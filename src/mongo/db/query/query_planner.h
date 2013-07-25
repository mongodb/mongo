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

#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * QueryPlanner's job is to provide an entry point to the query planning and optimization
     * process.
     */
    class QueryPlanner {
    public:
        /**
         * Outputs a series of possible solutions for the provided 'query' into 'out'.  Caller must
         * then decide which to run, if any.
         *
         * Caller owns the pointers in *out.
         */
        static void plan(const CanonicalQuery& query, vector<QuerySolution*> *out) {
        }
    };

}  // namespace mongo
