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

#include <set>
#include <boost/optional/optional.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

    struct PlanRankingDecision;
    struct QuerySolution;
    struct QuerySolutionNode;

    /**
     * TODO HK notes

     * cache should be LRU with some cap on size

     * write ops should invalidate but tell plan_cache there was a write op, don't enforce policy elsewhere,
       enforce here.

     * cache key is sort + query shape + projection

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

    // TODO: Replace with opaque type.
    typedef std::string PlanID;

    /**
     * A PlanCacheIndexTree is the meaty component of the data
     * stored in SolutionCacheData. It is a tree structure with
     * index tags that indicates to the access planner which indices
     * it should try to use.
     *
     * How a PlanCacheIndexTree is created:
     *   The query planner tags a match expression with indices. It
     *   then uses the tagged tree to create a PlanCacheIndexTree,
     *   using QueryPlanner::cacheDataFromTaggedTree. The PlanCacheIndexTree
     *   is isomorphic to the tagged match expression, and has matching
     *   index tags.
     *
     * How a PlanCacheIndexTree is used:
     *   When the query planner is planning from the cache, it uses
     *   the PlanCacheIndexTree retrieved from the cache in order to
     *   recreate index assignments. Specifically, a raw MatchExpression
     *   is tagged according to the index tags in the PlanCacheIndexTree.
     *   This is done by QueryPlanner::tagAccordingToCache.
     */
    struct PlanCacheIndexTree {
        PlanCacheIndexTree() : entry(NULL), index_pos(0) { }

        ~PlanCacheIndexTree() {
            for (vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                    it != children.end(); ++it) {
                delete *it;
            }
        }

        /**
         * Clone 'ie' and set 'this->entry' to be the clone.
         */
        void setIndexEntry(const IndexEntry& ie);

        /**
         * Make a deep copy.
         */
        PlanCacheIndexTree* clone() const;

        /**
         * For debugging.
         */
        std::string toString(int indents = 0) const;

        // Children owned here.
        std::vector<PlanCacheIndexTree*> children;

        // Owned here.
        boost::scoped_ptr<IndexEntry> entry;

        size_t index_pos;
    };

    /**
     * Data stored inside a QuerySolution which can subsequently be
     * used to create a cache entry. When this data is retrieved
     * from the cache, it is sufficient to reconstruct the original
     * QuerySolution.
     */
    struct SolutionCacheData {
        SolutionCacheData() :
            tree(NULL),
            solnType(USE_INDEX_TAGS_SOLN),
            wholeIXSolnDir(1) {
        }

        // Make a deep copy.
        SolutionCacheData* clone() const;

        // For debugging.
        std::string toString() const;

        // Owned here. If 'wholeIXSoln' is false, then 'tree'
        // can be used to tag an isomorphic match expression. If 'wholeIXSoln'
        // is true, then 'tree' is used to store the relevant IndexEntry.
        // If 'collscanSoln' is true, then 'tree' should be NULL.
        scoped_ptr<PlanCacheIndexTree> tree;

        enum SolutionType {
            // Indicates that the plan should use
            // the index as a proxy for a collection
            // scan (e.g. using index to provide sort).
            WHOLE_IXSCAN_SOLN,

            // The cached plan is a collection scan.
            COLLSCAN_SOLN,

            // Build the solution by using 'tree'
            // to tag the match expression.
            USE_INDEX_TAGS_SOLN
        } solnType;

        // The direction of the index scan used as
        // a proxy for a collection scan. Used only
        // for WHOLE_IXSCAN_SOLN.
        int wholeIXSolnDir;
    };

    class PlanCacheEntry;

    /**
     * Information returned from a get(...) query.
     */
    class CachedSolution {
    private:
        MONGO_DISALLOW_COPYING(CachedSolution);
    public:
        CachedSolution(const PlanCacheKey& key, const PlanCacheEntry& entry);
        ~CachedSolution();

        /**
         * Resolves index of winning solution.
         * Takes into account pinned and shunned plans.
         * Pinned plans take precendence over shunned plans.
         * If a plan is both pinned and shunned, it will be the winning plan.
         * The reason we provide the index into plannerData is to support the
         * notion of a backup plan in the multi plan runner. The cache solution
         * runner could go to the next solution after the winner index.
         */
        size_t getWinnerIndex() const;

        // Owned here.
        std::vector<SolutionCacheData*> plannerData;

        // An index into plannerData indicating the SolutionCacheData which should be
        // used to produce a backup solution in the case of a blocking sort.
        boost::optional<size_t> backupSoln;

        // Pin information
        bool pinned;

        // Index of pinned plan in plannerData.
        // Valid if pinned is true.
        size_t pinnedIndex;

        // Indexes of shunned plans.
        std::set<size_t> shunnedIndexes;

        // Key used to provide feedback on the entry.
        PlanCacheKey key;

        // For debugging.
        std::string toString() const;

        // We are extracting just enough information from the canonical
        // query. We could clone the canonical query but the following
        // items are all that is displayed to the user.
        BSONObj query;
        BSONObj sort;
        BSONObj projection;
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
         * Grabs any planner-specific data required from the solutions.
         * Takes ownership of the PlanRankingDecision that placed the plan in the cache.
         * XXX: what else should this take?
         */
        PlanCacheEntry(const std::vector<QuerySolution*>& solutions,
                   PlanRankingDecision* d);

        ~PlanCacheEntry();

        // For debugging.
        std::string toString() const;

        // Data provided to the planner to allow it to recreate the solutions this entry
        // represents. Each SolutionCacheData is fully owned here, so in order to return
        // it from the cache a deep copy is made and returned inside CachedSolution.
        std::vector<SolutionCacheData*> plannerData;

        // An index into plannerData indicating the SolutionCacheData which should be
        // used to produce a backup solution in the case of a blocking sort.
        boost::optional<size_t> backupSoln;

        // Why the best solution was picked.
        // TODO: Do we want to store other information like the other plans considered?
        boost::scoped_ptr<PlanRankingDecision> decision;

        // Annotations from cached runs.  The CachedSolutionRunner provides these stats about its
        // runs when they complete.  TODO: How many of these do we really want to keep?
        std::vector<PlanCacheEntryFeedback*> feedback;

        // Is this pinned in the cache?  If so, we will never remove it as a result of feedback.
        bool pinned;

        // Index of pinned plan in plannerData.
        // Valid if pinned is true.
        size_t pinnedIndex;

        // Indexes of shunned plans.
        std::set<size_t> shunnedIndexes;

        // XXX: Replace with copy of canonical query?
        BSONObj query;
        BSONObj sort;
        BSONObj projection;
    };

    /**
     * Caches the best solution to a query.  Aside from the (CanonicalQuery -> QuerySolution)
     * mapping, the cache contains information on why that mapping was made and statistics on the
     * cache entry's actual performance on subsequent runs.
     *
     */
    class PlanCache {
    private:
        MONGO_DISALLOW_COPYING(PlanCache);
    public:
        /**
         * Flush cache when the number of write operations since last
         * clear() reaches this limit.
         */
        static const int kPlanCacheMaxWriteOperations;

        /**
         * We don't want to cache every possible query. This function
         * encapsulates the criteria for what makes a canonical query
         * suitable for lookup/inclusion in the cache.
         */
        static bool shouldCacheQuery(const CanonicalQuery& query);

        /**
         * Generates a key for a normalized (for caching) canonical query
         * from the match expression and sort order.
         */
        static PlanCacheKey getPlanCacheKey(const CanonicalQuery& query);

        PlanCache() { }

        ~PlanCache();

        /**
         * Record solutions for query. Best plan is first element in list.
         * Each query in the cache will have more than 1 plan because we only
         * add queries which are considered by the multi plan runner (which happens
         * only when the query planner generates multiple candidate plans).
         *
         * Takes ownership of 'why'.
         *
         * If the mapping was added successfully, returns Status::OK().
         * If the mapping already existed or some other error occurred, returns another Status.
         */
        Status add(const CanonicalQuery& query,
                   const std::vector<QuerySolution*>& solns,
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
        Status get(const CanonicalQuery& query, CachedSolution** crOut) const;

        /**
         * Look up the cached data access for the provided key.
         *
         * If there is no entry in the cache for the 'query', returns an error Status.
         *
         * If there is an entry in the cache, populates 'crOut' and returns Status::OK().  Caller
         * owns '*crOut'.
         */
        Status get(const PlanCacheKey& key, CachedSolution** crOut) const;

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

        /**
         * Retrieves all plan cache keys
         */
        void getKeys(std::vector<PlanCacheKey>* keysOut) const;

        /**
         * Pins plan on a query in the cache. Subsequent cached solutions
         * will be generated based on the pinned plan.
         */
        Status pin(const PlanCacheKey& key, const PlanID& plan);

        /**
         * Unpins query. No-op if there is no plan pinned to the query.
         */
        Status unpin(const PlanCacheKey& key);

        /**
         * Adds user-defined plan.
         */
        Status addPlan(const PlanCacheKey& key, const BSONObj& details, PlanID* planOut);

        /**
         * Removes plan from cache entry.
         */
        Status shunPlan(const PlanCacheKey& key, const PlanID& plan);

        /**
         *  You must notify the cache if you are doing writes, as query plan utility will change.
         *  Cache is flushed after every 1000 notifications.
         */
        void notifyOfWriteOp();

    private:

        /**
         * Releases resources associated with each cache entry
         * and clears map.
         * Invoked by clear() and during destruction.
         */
        void _clear();

        unordered_map<PlanCacheKey, PlanCacheEntry*> _cache;

        /**
         * Protects _cache.
         */
        mutable boost::mutex _cacheMutex;

        /**
         * Counter for write notifications since initialization or last clear() invocation.
         * Starts at 0.
         */
        AtomicInt32 _writeOperations;
    };

}  // namespace mongo
