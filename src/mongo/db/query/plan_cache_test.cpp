/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <sstream>
#include <memory>
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    using std::auto_ptr;
    using std::stringstream;

    static const char* ns = "somebogusns";

    /**
     * Utility functions to create a CanonicalQuery
     */
    CanonicalQuery* canonicalize(const char* queryStr) {
        BSONObj queryObj = fromjson(queryStr);
        CanonicalQuery* cq;
        Status result = CanonicalQuery::canonicalize(ns, queryObj, &cq);
        ASSERT_OK(result);
        return cq;
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
            stringstream ss;
            ss << "failed to parse query: " << obj.toString()
               << ". Reason: " << status.toString();
            FAIL(ss.str());
        }
        MatchExpression* expr(status.getValue());
        return expr;
    }

    void assertEquivalent(const char* queryStr, const MatchExpression* expected, const MatchExpression* actual) {
        if (actual->equivalent(expected)) {
            return;
        }
        stringstream ss;
        ss << "Match expressions are not equivalent."
           << "\nOriginal query: " << queryStr
           << "\nExpected: " << expected->toString()
           << "\nActual: " << actual->toString();
        FAIL(ss.str());
    }

    /**
     * Test functions for shouldCacheQuery
     * Use these functions to assert which categories
     * of canonicalized queries are suitable for inclusion
     * in the planner cache.
     */
    void assertShouldCacheQuery(const CanonicalQuery& query) {
        if (shouldCacheQuery(query)) {
            return;
        }
        stringstream ss;
        ss << "Canonical query should be cacheable: " << query.toString();
        FAIL(ss.str());
    }

    void assertShouldNotCacheQuery(const CanonicalQuery& query) {
        if (!shouldCacheQuery(query)) {
            return;
        }
        stringstream ss;
        ss << "Canonical query should not be cacheable: " << query.toString();
        FAIL(ss.str());
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
     * Test functions for normalizeQueryForCache.
     * Cacheable queries need to go through an additional level of normalization beyong
     * what's already done in CanonicalQuery::normalizeTree.
     * The current behavior is to sort the internal nodes of the match expression
     * to ensure all operator/field name nodes are ordered the same way.
     */

    void testNormalizeQueryForCache(const char* queryStr, const char* expectedExprStr) {
        auto_ptr<CanonicalQuery> cq(canonicalize(queryStr));
        normalizeQueryForCache(cq.get());
        MatchExpression* me = cq->root();
        BSONObj expectedExprObj = fromjson(expectedExprStr);
        auto_ptr<MatchExpression> expectedExpr(parseMatchExpression(expectedExprObj));
        assertEquivalent(queryStr, expectedExpr.get(), me);
    }

    TEST(PlanCacheTest, NormalizeQueryForCache) {
        // Field names
        testNormalizeQueryForCache("{b: 1, a: 1}", "{a: 1, b: 1}");
        // Operator types
        testNormalizeQueryForCache("{a: {$gt: 5}, a: {$lt: 10}}}", "{a: {$lt: 10}, a: {$gt: 5}}");
        // Nested queries
        testNormalizeQueryForCache("{a: {$elemMatch: {c: 1, b:1}}}",
                                   "{a: {$elemMatch: {b: 1, c:1}}}");
    }

    /**
     * Test functions for getPlanCacheKey.
     * Cache keys are intentionally obfuscated and are meaningful only
     * within the current lifetime of the server process. Users should treat
     * plan cache keys as opaque.
     */
    void testGetPlanCacheKey(const char* queryStr, const char* sortStr,
                             const char *expectedStr) {
        auto_ptr<CanonicalQuery> cq(canonicalize(queryStr, sortStr, "{}"));
        PlanCacheKey key = getPlanCacheKey(*cq);
        PlanCacheKey expectedKey(expectedStr);
        if (key == expectedKey) {
            return;
        }
        stringstream ss;
        ss << "Unexpected plan cache key. Expected: " << expectedKey << ". Actual: " << key
           << ". Query: " << cq->toString();
        FAIL(ss.str());
    }

    TEST(PlanCacheTest, getPlanCacheKey) {
        // Generated cache keys should be treated as opaque to the user.
        // No sorts
        testGetPlanCacheKey("{}", "{}", "an");
        testGetPlanCacheKey("{$or: [{a: 1}, {b: 2}]}", "{}", "oreqaeqb");
        // With sort
        testGetPlanCacheKey("{}", "{a: 1}", "anaa");
        testGetPlanCacheKey("{}", "{a: -1}", "anda");
        testGetPlanCacheKey("{}", "{a: {$meta: 'textScore'}}", "anta");
    }

}  // namespace
