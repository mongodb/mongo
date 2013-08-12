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
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * QueryPlanner's job is to provide an entry point to the query planning and optimization
     * process.
     */
    class QueryPlanner {
    public:
        /**
         * Outputs a series of possible solutions for the provided 'query' into 'out'.  Uses the
         * provided indices to generate a solution.
         *
         * Caller owns pointers in *out.
         */
        static void plan(const CanonicalQuery& query,
                         const BSONObjSet& indexKeyPatterns,
                         vector<QuerySolution*>* out);

    private:
        /**
         * Returns true if the tree rooted at 'node' requires an index to answer the query.  There
         * is a default solution for every plan that is a collection scan + a filter for the full
         * query.  We can use this default solution when the query doesn't require an index.
         *
         * TODO: When we create plans with indices, we'll want to know which nodes require an index
         * and what the parents of those nodes are.
         */
        static bool requiresIndex(const MatchExpression* node);
    };

}  // namespace mongo
