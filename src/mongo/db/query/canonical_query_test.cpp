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

    using std::auto_ptr;
    using std::string;
    using std::unique_ptr;

    static const char* ns = "somebogusns";

    /**
     * Utility function to parse the given JSON as a MatchExpression and normalize the expression
     * tree.  Returns the resulting tree, or an error Status.
     */
    StatusWithMatchExpression parseNormalize(const std::string& queryStr) {
        // TODO Parsing a MatchExpression from a temporary BSONObj is invalid.  SERVER-18086.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(fromjson(queryStr));
        if (!swme.getStatus().isOK()) {
            return swme;
        }
        return StatusWithMatchExpression(CanonicalQuery::normalizeTree(swme.getValue()));
    }

    MatchExpression* parseMatchExpression(const BSONObj& obj) {
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj);
        if (!status.isOK()) {
            mongoutils::str::stream ss;
            ss << "failed to parse query: " << obj.toString()
               << ". Reason: " << status.getStatus().toString();
            FAIL(ss);
        }
        return status.getValue();
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

    void assertNotEquivalent(const char* queryStr,
                             const MatchExpression* expected,
                             const MatchExpression* actual) {
        if (!actual->equivalent(expected)) {
            return;
        }
        mongoutils::str::stream ss;
        ss << "Match expressions are equivalent."
           << "\nOriginal query: " << queryStr
           << "\nExpected: " << expected->toString()
           << "\nActual: " << actual->toString();
        FAIL(ss);
    }


    TEST(CanonicalQueryTest, IsValidText) {
        // Passes in default values for LiteParsedQuery.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Valid: regular TEXT.
        auto swme = parseNormalize("{$text: {$search: 's'}}");
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
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Valid: regular GEO_NEAR.
        auto swme = parseNormalize("{a: {$near: [0, 0]}}");
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
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: TEXT and GEO_NEAR.
        auto swme = parseNormalize("{$text: {$search: 's'}, a: {$near: [0, 0]}}");
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
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: TEXT and {$natural: 1} sort order.
        auto swme = parseNormalize("{$text: {$search: 's'}}");
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
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: TEXT and {$natural: -1} sort order.
        auto swme = parseNormalize("{$text: {$search: 's'}}");
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
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: TEXT and {$natural: -1} sort order.
        auto swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    // SERVER-14366
    TEST(CanonicalQueryTest, IsValidGeoNearNaturalSort) {
        // Passes in default values for LiteParsedQuery except for sort order.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        BSONObj sort = fromjson("{$natural: 1}");
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        sort, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: GEO_NEAR and {$natural: 1} sort order.
        auto swme = parseNormalize("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    // SERVER-14366
    TEST(CanonicalQueryTest, IsValidGeoNearNaturalHint) {
        // Passes in default values for LiteParsedQuery except for the hint.
        // Filter inside LiteParsedQuery is not used.
        LiteParsedQuery* lpqRaw;
        BSONObj hint = fromjson("{$natural: 1}");
        ASSERT_OK(LiteParsedQuery::make(ns, 0, 0, 0, fromjson("{}"), fromjson("{}"),
                                        fromjson("{}"), hint, fromjson("{}"),
                                        fromjson("{}"),
                                        false, // snapshot
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: GEO_NEAR and {$natural: 1} hint.
        auto swme = parseNormalize("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}");
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
                                        fromjson("{}"),
                                        snapshot,
                                        false, // explain
                                        &lpqRaw));
        auto_ptr<LiteParsedQuery> lpq(lpqRaw);

        auto_ptr<MatchExpression> me;

        // Invalid: TEXT and snapshot.
        auto swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get(), *lpq));
    }

    //
    // Tests for CanonicalQuery::sortTree
    //

    /**
     * Helper function for testing CanonicalQuery::sortTree().
     *
     * Verifies that sorting the expression 'unsortedQueryStr' yields an expression equivalent to
     * the expression 'sortedQueryStr'.
     */
    void testSortTree(const char* unsortedQueryStr, const char* sortedQueryStr) {
        BSONObj unsortedQueryObj = fromjson(unsortedQueryStr);
        unique_ptr<MatchExpression> unsortedQueryExpr(parseMatchExpression(unsortedQueryObj));

        BSONObj sortedQueryObj = fromjson(sortedQueryStr);
        unique_ptr<MatchExpression> sortedQueryExpr(parseMatchExpression(sortedQueryObj));

        // Sanity check that the unsorted expression is not equivalent to the sorted expression.
        assertNotEquivalent(unsortedQueryStr, unsortedQueryExpr.get(), sortedQueryExpr.get());

        // Sanity check that sorting the sorted expression is a no-op.
        {
            unique_ptr<MatchExpression> sortedQueryExprClone(parseMatchExpression(sortedQueryObj));
            CanonicalQuery::sortTree(sortedQueryExprClone.get());
            assertEquivalent(unsortedQueryStr, sortedQueryExpr.get(), sortedQueryExprClone.get());
        }

        // Test that sorting the unsorted expression yields the sorted expression.
        CanonicalQuery::sortTree(unsortedQueryExpr.get());
        assertEquivalent(unsortedQueryStr, unsortedQueryExpr.get(), sortedQueryExpr.get());
    }

    // Test that an EQ expression sorts before a GT expression.
    TEST(CanonicalQueryTest, SortTreeMatchTypeComparison) {
        testSortTree("{a: {$gt: 1}, a: 1}", "{a: 1, a: {$gt: 1}}");
    }

    // Test that an EQ expression on path "a" sorts before an EQ expression on path "b".
    TEST(CanonicalQueryTest, SortTreePathComparison) {
        testSortTree("{b: 1, a: 1}", "{a: 1, b: 1}");
        testSortTree("{'a.b': 1, a: 1}", "{a: 1, 'a.b': 1}");
        testSortTree("{'a.c': 1, 'a.b': 1}", "{'a.b': 1, 'a.c': 1}");
    }

    // Test that AND expressions sort according to their first differing child.
    TEST(CanonicalQueryTest, SortTreeChildComparison) {
        testSortTree("{$or: [{a: 1, c: 1}, {a: 1, b: 1}]}", "{$or: [{a: 1, b: 1}, {a: 1, c: 1}]}");
    }

    // Test that an AND with 2 children sorts before an AND with 3 children, if the first 2 children
    // are equivalent in both.
    TEST(CanonicalQueryTest, SortTreeNumChildrenComparison) {
        testSortTree("{$or: [{a: 1, b: 1, c: 1}, {a: 1, b: 1}]}",
                     "{$or: [{a: 1, b: 1}, {a: 1, b: 1, c: 1}]}");
    }

    //
    // Tests for CanonicalQuery::logicalRewrite
    //

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

    // Don't do anything with a double OR.
    TEST(CanonicalQueryTest, RewriteNoDoubleOr) {
        string queryStr = "{$or:[{a:1}, {b:1}], $or:[{c:1}, {d:1}], e:1}";
        BSONObj queryObj = fromjson(queryStr);
        auto_ptr<MatchExpression> base(parseMatchExpression(queryObj));
        auto_ptr<MatchExpression> rewrite(CanonicalQuery::logicalRewrite(base->shallowClone()));
        assertEquivalent(queryStr.c_str(), base.get(), rewrite.get());
    }

    // Do something with a single or.
    TEST(CanonicalQueryTest, RewriteSingleOr) {
        // Rewrite of this...
        string queryStr = "{$or:[{a:1}, {b:1}], e:1}";
        BSONObj queryObj = fromjson(queryStr);
        auto_ptr<MatchExpression> rewrite(CanonicalQuery::logicalRewrite(parseMatchExpression(queryObj)));

        // Should look like this.
        string rewriteStr = "{$or:[{a:1, e:1}, {b:1, e:1}]}";
        BSONObj rewriteObj = fromjson(rewriteStr);
        auto_ptr<MatchExpression> base(parseMatchExpression(rewriteObj));
        assertEquivalent(queryStr.c_str(), base.get(), rewrite.get());
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

}
