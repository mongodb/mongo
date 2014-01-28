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

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    static const char* ns = "somebogusns";

    /**
     * Utility function to parse the given JSON as a MatchExpression and normalize the expression
     * tree.  Returns the resulting tree, or an error Status.
     */
    StatusWithMatchExpression parseNormalize(const std::string& queryStr) {
        StatusWithMatchExpression swme = MatchExpressionParser::parse(fromjson(queryStr));
        if (!swme.getStatus().isOK()) {
            return swme;
        }
        return StatusWithMatchExpression(CanonicalQuery::normalizeTree(swme.getValue()));
    }

    TEST(CanonicalQueryTest, IsValidText) {
        // Passes in default values for LiteParsedQuery.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), false, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Valid: regular TEXT.
        swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Valid: TEXT inside OR.
        swme = parseNormalize(
            "{$or: ["
            "    {$text: {$search: 's'}},"
            "    {a: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Valid: TEXT outside NOR.
        swme = parseNormalize("{$text: {$search: 's'}, $nor: [{a: 1}, {b: 1}]}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: TEXT inside NOR.
        swme = parseNormalize("{$nor: [{$text: {$search: 's'}}, {a: 1}]}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: TEXT inside NOR.
        swme = parseNormalize(
            "{$nor: ["
            "    {$or: ["
            "        {$text: {$search: 's'}},"
            "        {a: 1}"
            "    ]},"
            "    {a: 2}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: >1 TEXT.
        swme = parseNormalize(
            "{$and: ["
            "    {$text: {$search: 's'}},"
            "    {$text: {$search: 't'}}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: >1 TEXT.
        swme = parseNormalize(
            "{$and: ["
            "    {$or: ["
            "        {$text: {$search: 's'}},"
            "        {a: 1}"
            "    ]},"
            "    {$or: ["
            "        {$text: {$search: 't'}},"
            "        {b: 1}"
            "    ]}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    TEST(CanonicalQueryTest, IsValidGeo) {
        // Passes in default values for LiteParsedQuery.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), false, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Valid: regular GEO_NEAR.
        swme = parseNormalize("{a: {$near: [0, 0]}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Valid: GEO_NEAR inside nested AND.
        swme = parseNormalize(
            "{$and: ["
            "    {$and: ["
            "        {a: {$near: [0, 0]}},"
            "        {b: 1}"
            "    ]},"
            "    {c: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: >1 GEO_NEAR.
        swme = parseNormalize(
            "{$and: ["
            "    {a: {$near: [0, 0]}},"
            "    {b: {$near: [0, 0]}}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: >1 GEO_NEAR.
        swme = parseNormalize(
            "{$and: ["
            "    {a: {$geoNear: [0, 0]}},"
            "    {b: {$near: [0, 0]}}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: >1 GEO_NEAR.
        swme = parseNormalize(
            "{$and: ["
            "    {$and: ["
            "        {a: {$near: [0, 0]}},"
            "        {b: 1}"
            "    ]},"
            "    {$and: ["
            "        {c: {$near: [0, 0]}},"
            "        {d: 1}"
            "    ]}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: GEO_NEAR inside NOR.
        swme = parseNormalize(
            "{$nor: ["
            "    {a: {$near: [0, 0]}},"
            "    {b: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: GEO_NEAR inside OR.
        swme = parseNormalize(
            "{$or: ["
            "    {a: {$near: [0, 0]}},"
            "    {b: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    TEST(CanonicalQueryTest, IsValidTextAndGeo) {
        // Passes in default values for LiteParsedQuery.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), false, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Invalid: TEXT and GEO_NEAR.
        swme = parseNormalize("{$text: {$search: 's'}, a: {$near: [0, 0]}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: TEXT and GEO_NEAR.
        swme = parseNormalize("{$text: {$search: 's'}, a: {$geoNear: [0, 0]}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));

        // Invalid: TEXT and GEO_NEAR.
        swme = parseNormalize(
            "{$or: ["
            "    {$text: {$search: 's'}},"
            "    {a: 1}"
            " ],"
            " b: {$near: [0, 0]}}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    TEST(CanonicalQueryTest, IsValidTextAndNaturalAscending) {
        // Passes in default values for LiteParsedQuery except for sort order.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        BSONObj sort = fromjson("{$natural: 1}");
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        sort, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), false, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Invalid: TEXT and {$natural: 1} sort order.
        swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    TEST(CanonicalQueryTest, IsValidTextAndNaturalDescending) {
        // Passes in default values for LiteParsedQuery except for sort order.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        BSONObj sort = fromjson("{$natural: -1}");
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        sort, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), false, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Invalid: TEXT and {$natural: -1} sort order.
        swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    TEST(CanonicalQueryTest, IsValidTextAndHint) {
        // Passes in default values for LiteParsedQuery except for hint.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        BSONObj hint = fromjson("{a: 1}");
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), hint, fromjson("{}"),
                                        fromjson("{}"), false, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Invalid: TEXT and {$natural: -1} sort order.
        swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    TEST(CanonicalQueryTest, IsValidTextAndSnapshot) {
        // Passes in default values for LiteParsedQuery except for snapshot.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        bool snapshot = true;
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), snapshot, &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Invalid: TEXT and snapshot.
        swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    /**
     * Utility function to create a CanonicalQuery
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

    void assertEquivalent(const char* queryStr,
                          const MatchExpression* expected,
                          const MatchExpression* actual) {
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

    /**
     * Test function for CanonicalQuery::normalize.
     */
    void testNormalizeQuery(const char* queryStr, const char* expectedExprStr) {
        auto_ptr<CanonicalQuery> cq(canonicalize(queryStr));
        MatchExpression* me = cq->root();
        BSONObj expectedExprObj = fromjson(expectedExprStr);
        auto_ptr<MatchExpression> expectedExpr(parseMatchExpression(expectedExprObj));
        assertEquivalent(queryStr, expectedExpr.get(), me);
    }

    TEST(CanonicalQueryTest, NormalizeQuerySort) {
        // Field names
        testNormalizeQuery("{b: 1, a: 1}", "{a: 1, b: 1}");
        // Operator types
        testNormalizeQuery("{a: {$gt: 5}, a: {$lt: 10}}}", "{a: {$lt: 10}, a: {$gt: 5}}");
        // Nested queries
        testNormalizeQuery("{a: {$elemMatch: {c: 1, b:1}}}",
                           "{a: {$elemMatch: {b: 1, c:1}}}");
    }

    TEST(CanonicalQueryTest, NormalizeQueryTree) {
        // Single-child $or elimination.
        testNormalizeQuery("{$or: [{b: 1}]}", "{b: 1}");
        // Single-child $and elimination.
        testNormalizeQuery("{$or: [{$and: [{a: 1}]}, {b: 1}]}", "{$or: [{a: 1}, {b: 1}]}");
        // $or absorbs $or children.
        testNormalizeQuery("{$or: [{a: 1}, {$or: [{b: 1}, {$or: [{c: 1}]}]}, {d: 1}]}",
                           "{$or: [{a: 1}, {b: 1}, {c: 1}, {d: 1}]}");
        // $and absorbs $and children.
        testNormalizeQuery("{$and: [{$and: [{a: 1}, {b: 1}]}, {c: 1}]}",
                           "{$and: [{a: 1}, {b: 1}, {c: 1}]}");
    }

    /**
     * Test functions for getPlanCacheKey.
     * Cache keys are intentionally obfuscated and are meaningful only
     * within the current lifetime of the server process. Users should treat
     * plan cache keys as opaque.
     */
    void testGetPlanCacheKey(const char* queryStr, const char* sortStr,
                             const char* projStr,
                             const char *expectedStr) {
        auto_ptr<CanonicalQuery> cq(canonicalize(queryStr, sortStr, projStr));
        const PlanCacheKey& key = cq->getPlanCacheKey();
        PlanCacheKey expectedKey(expectedStr);
        if (key == expectedKey) {
            return;
        }
        mongoutils::str::stream ss;
        ss << "Unexpected plan cache key. Expected: " << expectedKey << ". Actual: " << key
           << ". Query: " << cq->toString();
        FAIL(ss);
    }

    TEST(PlanCacheTest, GetPlanCacheKey) {
        // Generated cache keys should be treated as opaque to the user.
        // No sorts
        testGetPlanCacheKey("{}", "{}", "{}", "an");
        testGetPlanCacheKey("{$or: [{a: 1}, {b: 2}]}", "{}", "{}", "oreqaeqb");
        // With sort
        testGetPlanCacheKey("{}", "{a: 1}", "{}", "anaa");
        testGetPlanCacheKey("{}", "{a: -1}", "{}", "anda");
        testGetPlanCacheKey("{}", "{a: {$meta: 'textScore'}}", "{a: {$meta: 'textScore'}}",
                            "antap{ $meta: \"textScore\" }a");
        // With projection
        testGetPlanCacheKey("{}", "{}", "{a: 1}", "anp1a");
        testGetPlanCacheKey("{}", "{}", "{a: 0}", "anp0a");
        testGetPlanCacheKey("{}", "{}", "{a: 99}", "anp99a");
        testGetPlanCacheKey("{}", "{}", "{a: 'foo'}", "anp\"foo\"a");
        testGetPlanCacheKey("{}", "{}", "{a: {$slice: [3, 5]}}", "anp{ $slice: [ 3, 5 ] }a");
        testGetPlanCacheKey("{}", "{}", "{a: {$elemMatch: {x: 2}}}", "anp{ $elemMatch: { x: 2 } }a");
        testGetPlanCacheKey("{a: 1}", "{}", "{'a.$': 1}", "eqap1a.$");
        // With or-elimination and projection
        testGetPlanCacheKey("{$or: [{a: 1}]}", "{}", "{_id: 0, a: 1}", "eqap0_id1a");
        testGetPlanCacheKey("{$or: [{a: 1}]}", "{}", "{'a.$': 1}", "eqap1a.$");
    }

}
