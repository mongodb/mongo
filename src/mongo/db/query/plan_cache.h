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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * TODO: Debug commands:
     * 1. show canonical form of query
     * 2. show plans generated for query without (and with) cache
     * 3. print out cache.
     * 4. clear all elements from cache / otherwise manipulate cache.
     */

    /**
     * When the CachedPlanRunner runs a cached query, it can provide feedback to the cache.  This
     * feedback is available to anyone who retrieves that query in the future.
     */
    struct CachedSolutionFeedback {
        PlanStageStats* stats;
    };

    /**
     * A cached solution to a query.
     */
    struct CachedSolution {
        ~CachedSolution() {
            for (size_t i = 0; i < feedback.size(); ++i) {
                delete feedback[i];
            }
        }

        // The best solution for the CanonicalQuery.
        scoped_ptr<QuerySolution> solution;

        // Why the best solution was picked.
        scoped_ptr<PlanRankingDecision> decision;

        // Annotations from cached runs.
        // TODO: How many of these do we really want to keep?
        vector<CachedSolutionFeedback*> feedback;
    private:
        MONGO_DISALLOW_COPYING(CachedSolution);
    };

    /**
     * Caches the best solution to a query.  Aside from the (CanonicalQuery -> QuerySolution)
     * mapping, the cache contains information on why that mapping was made, and statistics on the
     * cache entry's actual performance on subsequent runs.
     */
    class PlanCache {
    public:
        /**
         * Get the (global) cache for the provided namespace.  Must not be held across yields.
         * As such, there is no locking required.
         */
        static PlanCache* get(const string& ns) { return NULL; }

        /**
         * Record 'solution' as the best plan for 'query' which was picked for reasons detailed in
         * 'why'.
         *
         * Takes ownership of all arguments.
         *
         * If the mapping was added successfully, returns true.
         * If the mapping already existed or some other error occurred, returns false;
         */
        bool add(CanonicalQuery* query, QuerySolution* solution, PlanRankingDecision* why) {
            return false;
        }

        /**
         * Look up the cached solution for the provided query.  If a cached solution exists, return
         * a copy of it which the caller then owns.  If no cached solution exists, returns NULL.
         *
         * TODO: Allow querying for exact query and querying for the shape of the query.
         */
        CachedSolution* get(const CanonicalQuery& query) {
            return NULL;
        }

        /**
         * When the CachedPlanRunner runs a plan out of the cache, we want to record data about the
         * plan's performance.  Cache takes ownership of 'feedback'.
         *
         * If the (query, solution) pair isn't in the cache, the cache deletes feedback and returns
         * false.  Otherwise, returns true.
         */
        bool feedback(const CanonicalQuery& query, const QuerySolution& solution,
                      const CachedSolutionFeedback* feedback) {
            return false;
        }

        /**
         * Remove the (query, solution) pair from our cache.  Returns true if the plan was removed,
         * false if it wasn't found.
         */
        bool remove(const CanonicalQuery& query, const QuerySolution& solution) {
            return false;
        }
    };

}  // namespace mongo
