/**
 *    Copyright (C) 2014 MongoDB Inc.
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

/**
 * This file contains tests for mongo/db/query/plan_cache.h
 */

#include "mongo/db/query/plan_cache.h"

#include <algorithm>
#include <boost/scoped_ptr.hpp>
#include <ostream>
#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::string;
    using std::vector;

    static const char* ns = "somebogusns";

    /**
     * Utility functions to create a CanonicalQuery
     */
    CanonicalQuery* canonicalize(const BSONObj& queryObj) {
        CanonicalQuery* cq;
        Status result = CanonicalQuery::canonicalize(ns, queryObj, &cq);
        ASSERT_OK(result);
        return cq;
    }

    CanonicalQuery* canonicalize(const char* queryStr) {
        BSONObj queryObj = fromjson(queryStr);
        return canonicalize(queryObj);
    }

    CanonicalQuery* canonicalize(const char* queryStr, const char* sortStr,
                                 const char* projStr) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projObj = fromjson(projStr);
        CanonicalQuery* cq;
        Status result = CanonicalQuery::canonicalize(ns, queryObj, sortObj,
                                                     projObj,
                                                     &cq);
        ASSERT_OK(result);
        return cq;
    }

     CanonicalQuery* canonicalize(const char* queryStr, const char* sortStr,
                                 const char* projStr,
                                 long long skip, long long limit,
                                 const char* hintStr,
                                 const char* minStr, const char* maxStr) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projObj = fromjson(projStr);
        BSONObj hintObj = fromjson(hintStr);
        BSONObj minObj = fromjson(minStr);
        BSONObj maxObj = fromjson(maxStr);
        CanonicalQuery* cq;
        Status result = CanonicalQuery::canonicalize(ns, queryObj, sortObj,
                                                     projObj,
                                                     skip, limit,
                                                     hintObj,
                                                     minObj, maxObj,
                                                     false, // snapshot
                                                     false, // explain
                                                     &cq);
        ASSERT_OK(result);
        return cq;
    }

     CanonicalQuery* canonicalize(const char* queryStr, const char* sortStr,
                                 const char* projStr,
                                 long long skip, long long limit,
                                 const char* hintStr,
                                 const char* minStr, const char* maxStr,
                                 bool snapshot,
                                 bool explain) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projObj = fromjson(projStr);
        BSONObj hintObj = fromjson(hintStr);
        BSONObj minObj = fromjson(minStr);
        BSONObj maxObj = fromjson(maxStr);
        CanonicalQuery* cq;
        Status result = CanonicalQuery::canonicalize(ns, queryObj, sortObj,
                                                     projObj,
                                                     skip, limit,
                                                     hintObj,
                                                     minObj, maxObj,
                                                     snapshot,
                                                     explain,
                                                     &cq);
        ASSERT_OK(result);
        return cq;
    }

   /**
    * Utility function to create MatchExpression
    */
    MatchExpression* parseMatchExpression(const BSONObj& obj) {
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj);
        if (!status.isOK()) {
            mongoutils::str::stream ss;
            ss << "failed to parse query: " << obj.toString()
               << ". Reason: " << status.toString();
            FAIL(ss);
        }
        MatchExpression* expr(status.getValue());
        return expr;
    }

    void assertEquivalent(const char* queryStr, const MatchExpression* expected, const MatchExpression* actual) {
        if (actual->equivalent(expected)) {
            return;
        }
        mongoutils::str::stream ss;
        ss << "Match expressions are not equivalent."
           << "\nOriginal query: " << queryStr
           << "\nExpected: " << expected->toString()
           << "\nActual: " << actual->toString();
        FAIL(ss);
    }

    //
    // Tests for CachedSolution
    //

    /**
     * Generator for vector of QuerySolution shared pointers.
     */
    struct GenerateQuerySolution {
        QuerySolution* operator()() const {
            auto_ptr<QuerySolution> qs(new QuerySolution());
            qs->cacheData.reset(new SolutionCacheData());
            qs->cacheData->solnType = SolutionCacheData::COLLSCAN_SOLN;
            qs->cacheData->tree.reset(new PlanCacheIndexTree());
            return qs.release();
        }
    };

    /**
     * Utility function to create a PlanRankingDecision
     */
    PlanRankingDecision* createDecision(size_t numPlans) {
        auto_ptr<PlanRankingDecision> why(new PlanRankingDecision());
        for (size_t i = 0; i < numPlans; ++i) {
            CommonStats common("COLLSCAN");
            auto_ptr<PlanStageStats> stats(new PlanStageStats(common, STAGE_COLLSCAN));
            stats->specific.reset(new CollectionScanStats());
            why->stats.mutableVector().push_back(stats.release());
            why->scores.push_back(0U);
            why->candidateOrder.push_back(i);
        }
        return why.release();
    }

    /**
     * Test functions for shouldCacheQuery
     * Use these functions to assert which categories
     * of canonicalized queries are suitable for inclusion
     * in the planner cache.
     */
    void assertShouldCacheQuery(const CanonicalQuery& query) {
        if (PlanCache::shouldCacheQuery(query)) {
            return;
        }
        mongoutils::str::stream ss;
        ss << "Canonical query should be cacheable: " << query.toString();
        FAIL(ss);
    }

    void assertShouldNotCacheQuery(const CanonicalQuery& query) {
        if (!PlanCache::shouldCacheQuery(query)) {
            return;
        }
        mongoutils::str::stream ss;
        ss << "Canonical query should not be cacheable: " << query.toString();
        FAIL(ss);
    }

    void assertShouldNotCacheQuery(const BSONObj& query) {
        auto_ptr<CanonicalQuery> cq(canonicalize(query));
        assertShouldNotCacheQuery(*cq);
    }

    void assertShouldNotCacheQuery(const char* queryStr) {
        auto_ptr<CanonicalQuery> cq(canonicalize(queryStr));
        assertShouldNotCacheQuery(*cq);
    }

    /**
     * Cacheable queries
     * These queries will be added to the cache with run-time statistics
     * and can be managed with the cache DB commands.
     */

    TEST(PlanCacheTest, ShouldCacheQueryBasic) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
        assertShouldCacheQuery(*cq);
    }

    TEST(PlanCacheTest, ShouldCacheQuerySort) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{}", "{a: -1}", "{_id: 0, a: 1}"));
        assertShouldCacheQuery(*cq);
    }

    /*
     * Non-cacheable queries.
     * These queries will be sent through the planning process everytime.
     */

    /**
     * Collection scan
     * This should normally be handled by the IDHack runner.
     */
    TEST(PlanCacheTest, ShouldNotCacheQueryCollectionScan) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{}"));
        assertShouldNotCacheQuery(*cq);
    }

    /**
     * Hint
     * A hinted query implies strong user preference for a particular index.
     * Therefore, not much point in caching.
     */
    TEST(PlanCacheTest, ShouldNotCacheQueryWithHint) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}", 0, 0, "{a: 1, b: 1}",
                                                 "{}", "{}"));
        assertShouldNotCacheQuery(*cq);
    }

    /**
     * Min queries are a specialized case of hinted queries
     */
    TEST(PlanCacheTest, ShouldNotCacheQueryWithMin) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}", 0, 0, "{}",
                                                 "{a: 100}", "{}"));
        assertShouldNotCacheQuery(*cq);
    }

    /**
     *  Max queries are non-cacheable for the same reasons as min queries.
     */
    TEST(PlanCacheTest, ShouldNotCacheQueryWithMax) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}", 0, 0, "{}",
                                                 "{}", "{a: 100}"));
        assertShouldNotCacheQuery(*cq);
    }

    /**
     * $geoWithin queries with legacy coordinates are cacheable as long as
     * the planner is able to come up with a cacheable solution.
     */
    TEST(PlanCacheTest, ShouldCacheQueryWithGeoWithinLegacyCoordinates) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: {$geoWithin: "
                                                 "{$box: [[-180, -90], [180, 90]]}}}"));
        assertShouldCacheQuery(*cq);
    }

    /**
     * $geoWithin queries with GeoJSON coordinates are supported by the index bounds builder.
     */
    TEST(PlanCacheTest, ShouldCacheQueryWithGeoWithinJSONCoordinates) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: {$geoWithin: "
                                                 "{$geometry: {type: 'Polygon', coordinates: "
                                                 "[[[0, 0], [0, 90], [90, 0], [0, 0]]]}}}}"));
        assertShouldCacheQuery(*cq);
    }

    /**
     * $geoWithin queries with both legacy and GeoJSON coordinates are cacheable.
     */
    TEST(PlanCacheTest, ShouldCacheQueryWithGeoWithinLegacyAndJSONCoordinates) {
        auto_ptr<CanonicalQuery> cq(canonicalize(
            "{$or: [{a: {$geoWithin: {$geometry: {type: 'Polygon', "
                                                 "coordinates: [[[0, 0], [0, 90], "
                                                                "[90, 0], [0, 0]]]}}}},"
                   "{a: {$geoWithin: {$box: [[-180, -90], [180, 90]]}}}]}"));
        assertShouldCacheQuery(*cq);
    }

    /**
     * $geoIntersects queries are always cacheable because they support GeoJSON coordinates only.
     */
    TEST(PlanCacheTest, ShouldCacheQueryWithGeoIntersects) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: {$geoIntersects: "
                                                 "{$geometry: {type: 'Point', coordinates: "
                                                 "[10.0, 10.0]}}}}"));
        assertShouldCacheQuery(*cq);
    }

    /**
     * $geoNear queries are cacheable because we are able to distinguish
     * between flat and spherical queries.
     */
    TEST(PlanCacheTest, ShouldNotCacheQueryWithGeoNear) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: {$geoNear: {$geometry: {type: 'Point',"
                                                 "coordinates: [0,0]}, $maxDistance:100}}}"));
        assertShouldCacheQuery(*cq);
    }

    /**
     * Explain queries are not-cacheable because of allPlans cannot
     * be accurately generated from stale cached stats in the plan cache for
     * non-winning plans.
     */
    TEST(PlanCacheTest, ShouldNotCacheQueryExplain) {
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}", 0, 0, "{}",
                                                 "{}", "{}", // min, max
                                                 false, // snapshot
                                                 true // explain
                                                 ));
        const LiteParsedQuery& pq = cq->getParsed();
        ASSERT_TRUE(pq.isExplain());
        assertShouldNotCacheQuery(*cq);
    }

    // Adding an empty vector of query solutions should fail.
    TEST(PlanCacheTest, AddEmptySolutions) {
        PlanCache planCache;
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
        std::vector<QuerySolution*> solns;
        boost::scoped_ptr<PlanRankingDecision> decision(createDecision(1U));
        ASSERT_NOT_OK(planCache.add(*cq, solns, decision.get()));
    }

    TEST(PlanCacheTest, AddValidSolution) {
        PlanCache planCache;
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
        QuerySolution qs;
        qs.cacheData.reset(new SolutionCacheData());
        qs.cacheData->tree.reset(new PlanCacheIndexTree());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);

        // Check if key is in cache before and after add().
        ASSERT_FALSE(planCache.contains(*cq));
        ASSERT_OK(planCache.add(*cq, solns, createDecision(1U)));

        ASSERT_TRUE(planCache.contains(*cq));
        ASSERT_EQUALS(planCache.size(), 1U);
    }

    TEST(PlanCacheTest, NotifyOfWriteOp) {
        PlanCache planCache;
        auto_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
        QuerySolution qs;
        qs.cacheData.reset(new SolutionCacheData());
        qs.cacheData->tree.reset(new PlanCacheIndexTree());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        ASSERT_OK(planCache.add(*cq, solns, createDecision(1U)));
        ASSERT_EQUALS(planCache.size(), 1U);

        // First (N - 1) write ops should have no effect on cache contents.
        for (int i = 0; i < (internalQueryCacheWriteOpsBetweenFlush - 1); ++i) {
            planCache.notifyOfWriteOp();
        }
        ASSERT_EQUALS(planCache.size(), 1U);

        // N-th notification will cause cache to be cleared.
        planCache.notifyOfWriteOp();
        ASSERT_EQUALS(planCache.size(), 0U);

        // Clearing the cache should reset the internal write
        // operation counter.
        // Repopulate cache. Write (N - 1) times.
        // Clear cache.
        // Add cache entry again.
        // After clearing and adding a new entry, the next write operation should not
        // clear the cache.
        ASSERT_OK(planCache.add(*cq, solns, createDecision(1U)));
        for (int i = 0; i < (internalQueryCacheWriteOpsBetweenFlush - 1); ++i) {
            planCache.notifyOfWriteOp();
        }
        ASSERT_EQUALS(planCache.size(), 1U);
        planCache.clear();
        ASSERT_OK(planCache.add(*cq, solns, createDecision(1U)));
        // Notification after clearing will not flush cache.
        planCache.notifyOfWriteOp();
        ASSERT_EQUALS(planCache.size(), 1U);
    }

    /**
     * Each test in the CachePlanSelectionTest suite goes through
     * the following flow:
     *
     * 1) Run QueryPlanner::plan on the query, with specified indices
     * available. This simulates the case in which we failed to plan from
     * the plan cache, and fell back on selecting a plan ourselves. The
     * enumerator will run, and cache data will be stashed into each solution
     * that it generates.
     *
     * 2) Use firstMatchingSolution to select one of the solutions generated
     * by QueryPlanner::plan. This simulates the multi plan runner picking
     * the "best solution".
     *
     * 3) The cache data stashed inside the "best solution" is used to
     * make a CachedSolution which looks exactly like the data structure that
     * would be returned from the cache. This simulates a plan cache hit.
     *
     * 4) Call QueryPlanner::planFromCache, passing it the CachedSolution.
     * This exercises the code which is able to map from a CachedSolution to
     * a full-blown QuerySolution. Finally, assert that the query solution
     * recovered from the cache is identical to the original "best solution".
     */
    class CachePlanSelectionTest : public mongo::unittest::Test {
    protected:
        void setUp() {
            cq = NULL;
            params.options = QueryPlannerParams::INCLUDE_COLLSCAN;
            addIndex(BSON("_id" << 1));
        }

        void tearDown() {
            delete cq;

            for (vector<QuerySolution*>::iterator it = solns.begin(); it != solns.end(); ++it) {
                delete *it;
            }
        }

        void addIndex(BSONObj keyPattern, bool multikey = false) {
            // The first false means not multikey.
            // The second false means not sparse.
            // The third arg is the index name and I am egotistical.
            params.indices.push_back(IndexEntry(keyPattern,
                                                multikey,
                                                false,
                                                "hari_king_of_the_stove",
                                                BSONObj()));
        }

        void addIndex(BSONObj keyPattern, bool multikey, bool sparse) {
            params.indices.push_back(IndexEntry(keyPattern,
                                                multikey,
                                                sparse,
                                                "note_to_self_dont_break_build",
                                                BSONObj()));
        }

        //
        // Execute planner.
        //

        void runQuery(BSONObj query) {
            runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
        }

        void runQuerySortProj(const BSONObj& query, const BSONObj& sort, const BSONObj& proj) {
            runQuerySortProjSkipLimit(query, sort, proj, 0, 0);
        }

        void runQuerySkipLimit(const BSONObj& query, long long skip, long long limit) {
            runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), skip, limit);
        }

        void runQueryHint(const BSONObj& query, const BSONObj& hint) {
            runQuerySortProjSkipLimitHint(query, BSONObj(), BSONObj(), 0, 0, hint);
        }

        void runQuerySortProjSkipLimit(const BSONObj& query,
                                       const BSONObj& sort, const BSONObj& proj,
                                       long long skip, long long limit) {
            runQuerySortProjSkipLimitHint(query, sort, proj, skip, limit, BSONObj());
        }

        void runQuerySortHint(const BSONObj& query, const BSONObj& sort, const BSONObj& hint) {
            runQuerySortProjSkipLimitHint(query, sort, BSONObj(), 0, 0, hint);
        }

        void runQueryHintMinMax(const BSONObj& query, const BSONObj& hint,
                                const BSONObj& minObj, const BSONObj& maxObj) {

            runQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj, false);
        }

        void runQuerySortProjSkipLimitHint(const BSONObj& query,
                                           const BSONObj& sort, const BSONObj& proj,
                                           long long skip, long long limit,
                                           const BSONObj& hint) {
            runQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj(), false);
        }

        void runQuerySnapshot(const BSONObj& query) {
            runQueryFull(query, BSONObj(), BSONObj(), 0, 0, BSONObj(), BSONObj(),
                         BSONObj(), true);
        }

        void runQueryFull(const BSONObj& query,
                          const BSONObj& sort, const BSONObj& proj,
                          long long skip, long long limit,
                          const BSONObj& hint,
                          const BSONObj& minObj,
                          const BSONObj& maxObj,
                          bool snapshot) {

            // Clean up any previous state from a call to runQueryFull
            delete cq;
            cq = NULL;

            for (vector<QuerySolution*>::iterator it = solns.begin(); it != solns.end(); ++it) {
                delete *it;
            }

            solns.clear();


            Status s = CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                                    minObj, maxObj, snapshot,
                                                    false, // explain
                                                    &cq);
            if (!s.isOK()) { cq = NULL; }
            ASSERT_OK(s);
            s = QueryPlanner::plan(*cq, params, &solns);
            ASSERT_OK(s);
        }

        //
        // Solution introspection.
        //

        void dumpSolutions(mongoutils::str::stream& ost) const {
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                ost << (*it)->toString() << '\n';
            }
        }

        /**
         * Returns number of generated solutions matching JSON.
         */
        size_t numSolutionMatches(const string& solnJson) const {
            BSONObj testSoln = fromjson(solnJson);
            size_t matches = 0;
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                QuerySolutionNode* root = (*it)->root.get();
                if (QueryPlannerTestLib::solutionMatches(testSoln, root)) {
                    ++matches;
                }
            }
            return matches;
        }

        /**
         * Verifies that the solution tree represented in json by 'solnJson' is
         * one of the solutions generated by QueryPlanner.
         *
         * The number of expected matches, 'numMatches', could be greater than
         * 1 if solutions differ only by the pattern of index tags on a filter.
         */
        void assertSolutionExists(const string& solnJson, size_t numMatches = 1) const {
            size_t matches = numSolutionMatches(solnJson);
            if (numMatches == matches) {
                return;
            }
            mongoutils::str::stream ss;
            ss << "expected " << numMatches << " matches for solution " << solnJson
               << " but got " << matches
               << " instead. all solutions generated: " << '\n';
            dumpSolutions(ss);
            FAIL(ss);
        }

        /**
         * Plan 'query' from the cache. A mock cache entry is created using
         * the cacheData stored inside the QuerySolution 'soln'.
         *
         * Does not take ownership of 'soln'.
         */
        QuerySolution* planQueryFromCache(const BSONObj& query, const QuerySolution& soln) const {
            return planQueryFromCache(query, BSONObj(), BSONObj(), soln);
        }

        /**
         * Plan 'query' from the cache with sort order 'sort' and
         * projection 'proj'. A mock cache entry is created using
         * the cacheData stored inside the QuerySolution 'soln'.
         *
         * Does not take ownership of 'soln'.
         */
        QuerySolution* planQueryFromCache(const BSONObj& query,
                                          const BSONObj& sort,
                                          const BSONObj& proj,
                                          const QuerySolution& soln) const {
            CanonicalQuery* cq;
            Status s = CanonicalQuery::canonicalize(ns, query, sort, proj, &cq);
            ASSERT_OK(s);
            scoped_ptr<CanonicalQuery> scopedCq(cq);
            cq = NULL;

            // Create a CachedSolution the long way..
            // QuerySolution -> PlanCacheEntry -> CachedSolution
            QuerySolution qs;
            qs.cacheData.reset(soln.cacheData->clone());
            std::vector<QuerySolution*> solutions;
            solutions.push_back(&qs);
            PlanCacheEntry entry(solutions, createDecision(1U));
            CachedSolution cachedSoln(ck, entry);

            QuerySolution *out, *backupOut;
            s = QueryPlanner::planFromCache(*scopedCq.get(), params, cachedSoln,
                                            &out, &backupOut);
            ASSERT_OK(s);
            std::auto_ptr<QuerySolution> cleanBackup(backupOut);

            return out;
        }

        /**
         * @param solnJson -- a json representation of a query solution.
         *
         * Returns the first solution matching 'solnJson', or fails if
         * no match is found.
         */
        QuerySolution* firstMatchingSolution(const string& solnJson) const {
            BSONObj testSoln = fromjson(solnJson);
            for (vector<QuerySolution*>::const_iterator it = solns.begin();
                    it != solns.end();
                    ++it) {
                QuerySolutionNode* root = (*it)->root.get();
                if (QueryPlannerTestLib::solutionMatches(testSoln, root)) {
                    return *it;
                }
            }

            mongoutils::str::stream ss;
            ss << "Could not find a match for solution " << solnJson
               << " All solutions generated: " << '\n';
            dumpSolutions(ss);
            FAIL(ss);

            return NULL;
        }

        /**
         * Assert that the QuerySolution 'trueSoln' matches the JSON-based representation
         * of the solution in 'solnJson'.
         *
         * Relies on solutionMatches() -- see query_planner_test_lib.h
         */
        void assertSolutionMatches(QuerySolution* trueSoln, const string& solnJson) const {
            BSONObj testSoln = fromjson(solnJson);
            if (!QueryPlannerTestLib::solutionMatches(testSoln, trueSoln->root.get())) {
                mongoutils::str::stream ss;
                ss << "Expected solution " << solnJson << " did not match true solution: "
                   << trueSoln->toString() << '\n';
                FAIL(ss);
            }
        }

        /**
         * Overloaded so that it is not necessary to specificy sort and project.
         */
        void assertPlanCacheRecoversSolution(const BSONObj& query, const string& solnJson) {
            assertPlanCacheRecoversSolution(query, BSONObj(), BSONObj(), solnJson);
        }

        /**
         * First, the solution matching 'solnJson' is retrieved from the vector
         * of solutions generated by QueryPlanner::plan. This solution is
         * then passed into planQueryFromCache(). Asserts that the solution
         * generated by QueryPlanner::planFromCache matches 'solnJson'.
         *
         * Must be called after calling one of the runQuery* methods.
         *
         * Together, 'query', 'sort', and 'proj' should specify the query which
         * was previously run using one of the runQuery* methods.
         */
        void assertPlanCacheRecoversSolution(const BSONObj& query,
                                             const BSONObj& sort,
                                             const BSONObj& proj,
                                             const string& solnJson) {
            QuerySolution* bestSoln = firstMatchingSolution(solnJson);
            QuerySolution* planSoln = planQueryFromCache(query, sort, proj, *bestSoln);
            assertSolutionMatches(planSoln, solnJson);
            delete planSoln;
        }

        /**
         * Check that the solution will not be cached. The planner will store
         * cache data inside non-cachable solutions, but will not do so for
         * non-cachable solutions. Therefore, we just have to check that
         * cache data is NULL.
         */
        void assertNotCached(const string& solnJson) {
            QuerySolution* bestSoln = firstMatchingSolution(solnJson);
            ASSERT(NULL != bestSoln);
            ASSERT(NULL == bestSoln->cacheData.get());
        }

        static const PlanCacheKey ck;

        BSONObj queryObj;
        CanonicalQuery* cq;
        QueryPlannerParams params;
        vector<QuerySolution*> solns;
    };

    const PlanCacheKey CachePlanSelectionTest::ck = "mock_cache_key";

    //
    // Equality
    //

    TEST_F(CachePlanSelectionTest, EqualityIndexScan) {
        addIndex(BSON("x" << 1));
        runQuery(BSON("x" << 5));

        assertPlanCacheRecoversSolution(BSON("x" << 5),
            "{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
    }

    TEST_F(CachePlanSelectionTest, EqualityIndexScanWithTrailingFields) {
        addIndex(BSON("x" << 1 << "y" << 1));
        runQuery(BSON("x" << 5));

        assertPlanCacheRecoversSolution(BSON("x" << 5),
            "{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: 1}}}}}");
    }

    //
    // Geo
    //

    TEST_F(CachePlanSelectionTest, Basic2DSphereNonNear) {
        addIndex(BSON("a" << "2dsphere"));
        BSONObj query;

        query = fromjson("{a: {$geoIntersects: {$geometry: {type: 'Point',"
                                                           "coordinates: [10.0, 10.0]}}}}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

        query = fromjson("{a : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");
    }

    TEST_F(CachePlanSelectionTest, Basic2DSphereGeoNear) {
        addIndex(BSON("a" << "2dsphere"));
        BSONObj query;

        query = fromjson("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query, "{geoNear2dsphere: {a: '2dsphere'}}");

        query = fromjson("{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
                                          "$maxDistance:100}}}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query, "{geoNear2dsphere: {a: '2dsphere'}}");
    }

    TEST_F(CachePlanSelectionTest, Basic2DSphereGeoNearReverseCompound) {
        addIndex(BSON("x" << 1));
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        BSONObj query = fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query, "{geoNear2dsphere: {x: 1, a: '2dsphere'}}");
    }

    TEST_F(CachePlanSelectionTest, TwoDSphereNoGeoPred) {
        addIndex(BSON("x" << 1 << "a" << "2dsphere"));
        runQuery(BSON("x" << 1));
        assertPlanCacheRecoversSolution(BSON("x" << 1),
            "{fetch: {node: {ixscan: {pattern: {x: 1, a: '2dsphere'}}}}}");
    }

    TEST_F(CachePlanSelectionTest, Or2DSphereNonNear) {
        addIndex(BSON("a" << "2dsphere"));
        addIndex(BSON("b" << "2dsphere"));
        BSONObj query = fromjson("{$or: [ {a: {$geoIntersects: {$geometry: {type: 'Point', coordinates: [10.0, 10.0]}}}},"
                                 " {b: {$geoWithin: { $centerSphere: [[ 10, 20 ], 0.01 ] } }} ]}");

        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}},"
                          "{fetch: {node: {ixscan: {pattern: {b: '2dsphere'}}}}}]}}");
    }

    //
    // tree operations
    //

    TEST_F(CachePlanSelectionTest, TwoPredicatesAnding) {
        addIndex(BSON("x" << 1));
        BSONObj query = fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
    }

    TEST_F(CachePlanSelectionTest, SimpleOr) {
        addIndex(BSON("a" << 1));
        BSONObj query = fromjson("{$or: [{a: 20}, {a: 21}]}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
    }

    TEST_F(CachePlanSelectionTest, OrWithAndChild) {
        addIndex(BSON("a" << 1));
        BSONObj query = fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {filter: null, node: {or: {nodes: ["
                "{ixscan: {filter: null, pattern: {a: 1}}}, "
                "{fetch: {filter: {b: 7}, node: {ixscan: "
                "{filter: null, pattern: {a: 1}}}}}]}}}}");
    }

    TEST_F(CachePlanSelectionTest, AndWithUnindexedOrChild) {
        addIndex(BSON("a" << 1));
        BSONObj query = fromjson("{a:20, $or: [{b:1}, {c:7}]}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, node: "
                "{ixscan: {filter: null, pattern: {a: 1}}}}}");
    }


    TEST_F(CachePlanSelectionTest, AndWithOrWithOneIndex) {
        addIndex(BSON("b" << 1));
        addIndex(BSON("a" << 1));
        BSONObj query = fromjson("{$or: [{b:1}, {c:7}], a:20}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, "
                "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    //
    // Sort orders
    //

    // SERVER-1205.
    TEST_F(CachePlanSelectionTest, MergeSort) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));

        BSONObj query = fromjson("{$or: [{a:1}, {b:1}]}");
        BSONObj sort = BSON("c" << 1);
        runQuerySortProj(query, sort, BSONObj());

        assertPlanCacheRecoversSolution(query, sort, BSONObj(),
            "{fetch: {node: {mergeSort: {nodes: "
                "[{ixscan: {pattern: {a: 1, c: 1}}}, {ixscan: {pattern: {b: 1, c: 1}}}]}}}}");
    }

    // SERVER-1205 as well.
    TEST_F(CachePlanSelectionTest, NoMergeSortIfNoSortWanted) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));

        BSONObj query = fromjson("{$or: [{a:1}, {b:1}]}");
        runQuerySortProj(query, BSONObj(), BSONObj());

        assertPlanCacheRecoversSolution(query, BSONObj(), BSONObj(),
            "{fetch: {filter: null, node: {or: {nodes: ["
                "{ixscan: {filter: null, pattern: {a: 1, c: 1}}}, "
                "{ixscan: {filter: null, pattern: {b: 1, c: 1}}}]}}}}");
    }

    // Disabled: SERVER-10801.
    /*
    TEST_F(CachePlanSelectionTest, SortOnGeoQuery) {
        addIndex(BSON("timestamp" << -1 << "position" << "2dsphere"));
        BSONObj query = fromjson("{position: {$geoWithin: {$geometry: {type: \"Polygon\", "
                                 "coordinates: [[[1, 1], [1, 90], [180, 90], "
                                 "[180, 1], [1, 1]]]}}}}");
        BSONObj sort = fromjson("{timestamp: -1}");
        runQuerySortProj(query, sort, BSONObj());

        assertPlanCacheRecoversSolution(query, sort, BSONObj(),
            "{fetch: {node: {ixscan: {pattern: {timestamp: -1, position: '2dsphere'}}}}}");
    }
    */

    // SERVER-9257
    TEST_F(CachePlanSelectionTest, CompoundGeoNoGeoPredicate) {
        addIndex(BSON("creationDate" << 1 << "foo.bar" << "2dsphere"));
        BSONObj query = fromjson("{creationDate: {$gt: 7}}");
        BSONObj sort = fromjson("{creationDate: 1}");
        runQuerySortProj(query, sort, BSONObj());

        assertPlanCacheRecoversSolution(query, sort, BSONObj(),
            "{fetch: {node: {ixscan: {pattern: {creationDate: 1, 'foo.bar': '2dsphere'}}}}}");
    }

    TEST_F(CachePlanSelectionTest, ReverseScanForSort) {
        addIndex(BSON("_id" << 1));
        runQuerySortProj(BSONObj(), fromjson("{_id: -1}"), BSONObj());
        assertPlanCacheRecoversSolution(BSONObj(), fromjson("{_id: -1}"), BSONObj(),
            "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {_id: 1}}}}}");
    }

    //
    // Caching collection scans.
    //

    TEST_F(CachePlanSelectionTest, CollscanNoUsefulIndices) {
        addIndex(BSON("a" << 1 << "b" << 1));
        addIndex(BSON("c" << 1));
        runQuery(BSON("b" << 4));
        assertPlanCacheRecoversSolution(BSON("b" << 4),
            "{cscan: {filter: {b: 4}, dir: 1}}");
    }

    TEST_F(CachePlanSelectionTest, CollscanOrWithoutEnoughIndices) {
        addIndex(BSON("a" << 1));
        BSONObj query =fromjson("{$or: [{a: 20}, {b: 21}]}");
        runQuery(query);
        assertPlanCacheRecoversSolution(query,
            "{cscan: {filter: {$or:[{a:20},{b:21}]}, dir: 1}}");
    }

    TEST_F(CachePlanSelectionTest, CollscanMergeSort) {
        addIndex(BSON("a" << 1 << "c" << 1));
        addIndex(BSON("b" << 1 << "c" << 1));

        BSONObj query = fromjson("{$or: [{a:1}, {b:1}]}");
        BSONObj sort = BSON("c" << 1);
        runQuerySortProj(query, sort, BSONObj());

        assertPlanCacheRecoversSolution(query, sort, BSONObj(),
            "{sort: {pattern: {c: 1}, limit: 0, node: {cscan: {dir: 1}}}}");
    }

    //
    // Check queries that, at least for now, are not cached.
    //

    TEST_F(CachePlanSelectionTest, GeoNear2DNotCached) {
        addIndex(BSON("a" << "2d"));
        runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
        assertNotCached("{geoNear2d: {a: '2d'}}");
    }

    TEST_F(CachePlanSelectionTest, MinNotCached) {
        addIndex(BSON("a" << 1));
        runQueryHintMinMax(BSONObj(), BSONObj(), fromjson("{a: 1}"), BSONObj());
        assertNotCached("{fetch: {filter: null, "
                           "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(CachePlanSelectionTest, MaxNotCached) {
        addIndex(BSON("a" << 1));
        runQueryHintMinMax(BSONObj(), BSONObj(), BSONObj(), fromjson("{a: 1}"));
        assertNotCached("{fetch: {filter: null, "
                            "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    TEST_F(CachePlanSelectionTest, NaturalHintNotCached) {
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));
        runQuerySortHint(BSON("a" << 1), BSON("b" << 1), BSON("$natural" << 1));
        assertNotCached("{sort: {pattern: {b: 1}, limit: 0, node: "
                            "{cscan: {filter: {a: 1}, dir: 1}}}}");
    }

    TEST_F(CachePlanSelectionTest, HintValidNotCached) {
        addIndex(BSON("a" << 1));
        runQueryHint(BSONObj(), fromjson("{a: 1}"));
        assertNotCached("{fetch: {filter: null, "
                            "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
    }

    //
    // Queries using '2d' indices are not cached.
    //

    TEST_F(CachePlanSelectionTest, Basic2DNonNearNotCached) {
        addIndex(BSON("a" << "2d"));
        BSONObj query;

        // Polygon
        query = fromjson("{a : { $within: { $polygon : [[0,0], [2,0], [4,0]] } }}");
        runQuery(query);
        assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

        // Center
        query = fromjson("{a : { $within : { $center : [[ 5, 5 ], 7 ] } }}");
        runQuery(query);
        assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

        // Centersphere
        query = fromjson("{a : { $within : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}");
        runQuery(query);
        assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

        // Within box.
        query = fromjson("{a : {$within: {$box : [[0,0],[9,9]]}}}");
        runQuery(query);
        assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");
    }

    TEST_F(CachePlanSelectionTest, Or2DNonNearNotCached) {
        addIndex(BSON("a" << "2d"));
        addIndex(BSON("b" << "2d"));
        BSONObj query = fromjson("{$or: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
                                        " {b : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}");

        runQuery(query);
        assertNotCached("{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}},"
                                      "{fetch: {node: {ixscan: {pattern: {b: '2d'}}}}}]}}");
    }

}  // namespace
