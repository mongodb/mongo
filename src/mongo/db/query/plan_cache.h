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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"

namespace mongo {

    struct PlanRankingDecision;
    struct QuerySolution;
    struct QuerySolutionNode;

    /**
     * TODO: Debug commands:
     * 1. show canonical form of query
     * 2. show plans generated for query without (and with) cache
     * 3. print out cache.
     * 4. clear all elements from cache
     * 5. pin and unpin cached solutions
     */

    /**
     * TODO HK notes

     * must cache query w/shape, not exact data.

     * cache should be LRU with some cap on size

     * order doesn't matter: ensure that cache entry for query={a:1, b:1} same for query={b:1, a:1}

     * cache whenever possible

     * write ops should invalidate but tell plan_cache there was a write op, don't enforce policy elsewhere,
       enforce here.

     * cache key is sort + query shape

     * {x:1} and {x:{$gt:7}} not same shape for now -- operator matters
     */

    /**
     * When the CachedPlanRunner runs a cached query, it can provide feedback to the cache.  This
     * feedback is available to anyone who retrieves that query in the future.
     */
    struct PlanCacheEntryFeedback {
        // How well did the cached plan perform?
        boost::scoped_ptr<PlanStageStats> stats;
    };

    // TODO: Is this binary data really?
    typedef std::string PlanCacheKey;

    /**
     * We don't want to cache every possible query. This function
     * encapsulates the criteria for what makes a canonical query
     * suitable for lookup/inclusion in the cache.
     */
    bool shouldCacheQuery(const CanonicalQuery& query);

    /**
     * Normalizes canonical query for caching.
     * Current sorts nodes of internal match expression.
     * Not to be confused with internal function CanonicalQuery::normalizeTree()
     */
    void normalizeQueryForCache(CanonicalQuery* queryOut);

    /**
     * Generates a key for a normalized (for caching) canonical query
     * from the match expression and sort order.
     */
    PlanCacheKey getPlanCacheKey(const CanonicalQuery& query);

    /**
     * Information returned from a get(...) query.
     */
    struct CachedSolution {
        // Owned here.
        // XXX: what type is this?  it's really a tree where the node is a tag on a matchexpression.
        void* plannerData;

        // Key used to provide feedback on the entry.
        PlanCacheKey key;

        // For debugging.
        std::string toString() const;

        // XXX: no copying
    };

    /**
     * Used internally by the cache to track entries and their performance over time.
     */
    class PlanCacheEntry {
    private:
        MONGO_DISALLOW_COPYING(PlanCacheEntry);
    public:
        // TODO: Do we want to store more information about the query here?

        /**
         * Create a new PlanCacheEntry.
         * Grabs any planner-specific data required from the solution.
         * Takes ownership of the PlanRankingDecision that placed the plan in the cache.
         * XXX: what else should this take?
         */
        PlanCacheEntry(const QuerySolution& s,
                   PlanRankingDecision* d);

        ~PlanCacheEntry();

        // For debugging.
        std::string toString() const;

        // Data provided to the planner to allow it to recreate the solution this entry represents.
        // TODO: This is the same thing as the TBD type in CachedSolution.
        // CachedSolution must be fully owned so we'll copy this to return it.
        void* plannerData;

        // Why the best solution was picked.
        // TODO: Do we want to store other information like the other plans considered?
        boost::scoped_ptr<PlanRankingDecision> decision;

        // Annotations from cached runs.  The CachedSolutionRunner provides these stats about its
        // runs when they complete.  TODO: How many of these do we really want to keep?
        std::vector<PlanCacheEntryFeedback*> feedback;

        // Is this pinned in the cache?  If so, we will never remove it as a result of feedback.
        bool pinned;
    };

    /**
     * Caches the best solution to a query.  Aside from the (CanonicalQuery -> QuerySolution)
     * mapping, the cache contains information on why that mapping was made and statistics on the
     * cache entry's actual performance on subsequent runs.
     *
     * XXX: lock in here!  this can be called by many threads at the same time since the only above-this
     * locking required is a read lock on the ns.
     */
    class PlanCache {
    private:
        MONGO_DISALLOW_COPYING(PlanCache);
    public:
        PlanCache() { }

        ~PlanCache();

        /**
         * Record 'solution' as the best plan for 'query' which was picked for reasons detailed in
         * 'why'.
         *
         * Takes ownership of 'why'.
         *
         * If the mapping was added successfully, returns Status::OK().
         * If the mapping already existed or some other error occurred, returns another Status.
         */
        Status add(const CanonicalQuery& query,
                   const QuerySolution& soln,
                   PlanRankingDecision* why);

        /**
         * Look up the cached data access for the provided 'query'.  Used by the query planner
         * to shortcut planning.
         *
         * If there is no entry in the cache for the 'query', returns an error Status.
         *
         * If there is an entry in the cache, populates 'crOut' and returns Status::OK().  Caller
         * owns '*crOut'.
         */
        Status get(const CanonicalQuery& query, CachedSolution** crOut);

        /**
         * When the CachedPlanRunner runs a plan out of the cache, we want to record data about the
         * plan's performance.  The CachedPlanRunner calls feedback(...) at the end of query
         * execution in order to do this.
         *
         * Cache takes ownership of 'feedback'.
         *
         * If the entry corresponding to 'ck' isn't in the cache anymore, the feedback is ignored
         * and an error Status is returned.
         *
         * If the entry corresponding to 'ck' still exists, 'feedback' is added to the run
         * statistics about the plan.  Status::OK() is returned.
         */
        Status feedback(const PlanCacheKey& ck, PlanCacheEntryFeedback* feedback);

        /**
         * Remove the entry corresponding to 'ck' from the cache.  Returns Status::OK() if the plan
         * was present and removed and an error status otherwise.
         */
        Status remove(const PlanCacheKey& ck);

        /**
         * Remove *all* entries.
         */
        void clear();

    private:
        unordered_map<PlanCacheKey, PlanCacheEntry*> _cache;
    };

}  // namespace mongo
