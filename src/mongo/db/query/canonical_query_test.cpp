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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using unittest::assertGet;

static const NamespaceString nss("testdb.testcoll");

/**
 * Helper function to parse the given BSON object as a MatchExpression, checks the status,
 * and return the MatchExpression*.
 */
MatchExpression* parseMatchExpression(const BSONObj& obj) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj, ExtensionsCallbackNoop(), collator);
    if (!status.isOK()) {
        mongoutils::str::stream ss;
        ss << "failed to parse query: " << obj.toString()
           << ". Reason: " << status.getStatus().toString();
        FAIL(ss);
    }

    return status.getValue().release();
}

/**
 * Helper function which parses and normalizes 'queryStr', and returns whether the given
 * (expression tree, query request) tuple passes CanonicalQuery::isValid().
 * Returns Status::OK() if the tuple is valid, else returns an error Status.
 */
Status isValid(const std::string& queryStr, const QueryRequest& qrRaw) {
    BSONObj queryObj = fromjson(queryStr);
    unique_ptr<MatchExpression> me(CanonicalQuery::normalizeTree(parseMatchExpression(queryObj)));
    return CanonicalQuery::isValid(me.get(), qrRaw);
}

void assertEquivalent(const char* queryStr,
                      const MatchExpression* expected,
                      const MatchExpression* actual) {
    if (actual->equivalent(expected)) {
        return;
    }
    mongoutils::str::stream ss;
    ss << "Match expressions are not equivalent."
       << "\nOriginal query: " << queryStr << "\nExpected: " << expected->toString()
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
       << "\nOriginal query: " << queryStr << "\nExpected: " << expected->toString()
       << "\nActual: " << actual->toString();
    FAIL(ss);
}


TEST(CanonicalQueryTest, IsValidText) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    ASSERT_OK(qr->validate());

    // Valid: regular TEXT.
    ASSERT_OK(isValid("{$text: {$search: 's'}}", *qr));

    // Valid: TEXT inside OR.
    ASSERT_OK(
        isValid("{$or: ["
                "    {$text: {$search: 's'}},"
                "    {a: 1}"
                "]}",
                *qr));

    // Valid: TEXT outside NOR.
    ASSERT_OK(isValid("{$text: {$search: 's'}, $nor: [{a: 1}, {b: 1}]}", *qr));

    // Invalid: TEXT inside NOR.
    ASSERT_NOT_OK(isValid("{$nor: [{$text: {$search: 's'}}, {a: 1}]}", *qr));

    // Invalid: TEXT inside NOR.
    ASSERT_NOT_OK(
        isValid("{$nor: ["
                "    {$or: ["
                "        {$text: {$search: 's'}},"
                "        {a: 1}"
                "    ]},"
                "    {a: 2}"
                "]}",
                *qr));

    // Invalid: >1 TEXT.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {$text: {$search: 's'}},"
                "    {$text: {$search: 't'}}"
                "]}",
                *qr));

    // Invalid: >1 TEXT.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {$or: ["
                "        {$text: {$search: 's'}},"
                "        {a: 1}"
                "    ]},"
                "    {$or: ["
                "        {$text: {$search: 't'}},"
                "        {b: 1}"
                "    ]}"
                "]}",
                *qr));
}

TEST(CanonicalQueryTest, IsValidTextTailable) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setTailable(true);
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and tailable.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(CanonicalQueryTest, IsValidGeo) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    ASSERT_OK(qr->validate());

    // Valid: regular GEO_NEAR.
    ASSERT_OK(isValid("{a: {$near: [0, 0]}}", *qr));

    // Valid: GEO_NEAR inside nested AND.
    ASSERT_OK(
        isValid("{$and: ["
                "    {$and: ["
                "        {a: {$near: [0, 0]}},"
                "        {b: 1}"
                "    ]},"
                "    {c: 1}"
                "]}",
                *qr));

    // Invalid: >1 GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {a: {$near: [0, 0]}},"
                "    {b: {$near: [0, 0]}}"
                "]}",
                *qr));

    // Invalid: >1 GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {a: {$geoNear: [0, 0]}},"
                "    {b: {$near: [0, 0]}}"
                "]}",
                *qr));

    // Invalid: >1 GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {$and: ["
                "        {a: {$near: [0, 0]}},"
                "        {b: 1}"
                "    ]},"
                "    {$and: ["
                "        {c: {$near: [0, 0]}},"
                "        {d: 1}"
                "    ]}"
                "]}",
                *qr));

    // Invalid: GEO_NEAR inside NOR.
    ASSERT_NOT_OK(
        isValid("{$nor: ["
                "    {a: {$near: [0, 0]}},"
                "    {b: 1}"
                "]}",
                *qr));

    // Invalid: GEO_NEAR inside OR.
    ASSERT_NOT_OK(
        isValid("{$or: ["
                "    {a: {$near: [0, 0]}},"
                "    {b: 1}"
                "]}",
                *qr));
}

TEST(CanonicalQueryTest, IsValidTextAndGeo) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and GEO_NEAR.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}, a: {$near: [0, 0]}}", *qr));

    // Invalid: TEXT and GEO_NEAR.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}, a: {$geoNear: [0, 0]}}", *qr));

    // Invalid: TEXT and GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$or: ["
                "    {$text: {$search: 's'}},"
                "    {a: 1}"
                " ],"
                " b: {$near: [0, 0]}}",
                *qr));
}

TEST(CanonicalQueryTest, IsValidTextAndNaturalAscending) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and {$natural: 1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(CanonicalQueryTest, IsValidTextAndNaturalDescending) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(fromjson("{$natural: -1}"));
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and {$natural: -1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(CanonicalQueryTest, IsValidTextAndHint) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setHint(fromjson("{a: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and {$natural: -1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

// SERVER-14366
TEST(CanonicalQueryTest, IsValidGeoNearNaturalSort) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: GEO_NEAR and {$natural: 1} sort order.
    ASSERT_NOT_OK(isValid("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}", *qr));
}

// SERVER-14366
TEST(CanonicalQueryTest, IsValidGeoNearNaturalHint) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: GEO_NEAR and {$natural: 1} hint.
    ASSERT_NOT_OK(isValid("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}", *qr));
}

TEST(CanonicalQueryTest, IsValidTextAndSnapshot) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSnapshot(true);
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and snapshot.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(CanonicalQueryTest, IsValidSortKeyMetaProjection) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Passing a sortKey meta-projection without a sort is an error.
    {
        const bool isExplain = false;
        auto qr = assertGet(QueryRequest::makeFromFindCommand(
            nss, fromjson("{find: 'testcoll', projection: {foo: {$meta: 'sortKey'}}}"), isExplain));
        auto cq = CanonicalQuery::canonicalize(
            txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_NOT_OK(cq.getStatus());
    }

    // Should be able to successfully create a CQ when there is a sort.
    {
        const bool isExplain = false;
        auto qr = assertGet(QueryRequest::makeFromFindCommand(
            nss,
            fromjson("{find: 'testcoll', projection: {foo: {$meta: 'sortKey'}}, sort: {bar: 1}}"),
            isExplain));
        auto cq = CanonicalQuery::canonicalize(
            txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(cq.getStatus());
    }
}

TEST(CanonicalQueryTest, IsValidNaturalSortIndexHint) {
    const bool isExplain = false;
    auto qr = assertGet(QueryRequest::makeFromFindCommand(
        nss, fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {a: 1}}"), isExplain));

    // Invalid: {$natural: 1} sort order and index hint.
    ASSERT_NOT_OK(isValid("{}", *qr));
}

TEST(CanonicalQueryTest, IsValidNaturalSortNaturalHint) {
    const bool isExplain = false;
    auto qr = assertGet(QueryRequest::makeFromFindCommand(
        nss, fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {$natural: 1}}"), isExplain));

    // Valid: {$natural: 1} sort order and {$natural: 1} hint.
    ASSERT_OK(isValid("{}", *qr));
}

TEST(CanonicalQueryTest, IsValidNaturalSortNaturalHintDifferentDirections) {
    const bool isExplain = false;
    auto qr = assertGet(QueryRequest::makeFromFindCommand(
        nss, fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {$natural: -1}}"), isExplain));

    // Invalid: {$natural: 1} sort order and {$natural: -1} hint.
    ASSERT_NOT_OK(isValid("{}", *qr));
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

/**
 * Utility function to create a CanonicalQuery
 */
unique_ptr<CanonicalQuery> canonicalize(const char* queryStr) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson(queryStr));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                             const char* sortStr,
                                             const char* projStr) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson(queryStr));
    qr->setSort(fromjson(sortStr));
    qr->setProj(fromjson(projStr));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

/**
 * Test that CanonicalQuery::isIsolated() returns correctly.
 */
TEST(CanonicalQueryTest, IsIsolatedReturnsTrueWithIsolated) {
    unique_ptr<CanonicalQuery> cq = canonicalize("{$isolated: 1, x: 3}");
    ASSERT_TRUE(cq->isIsolated());
}

TEST(CanonicalQueryTest, IsIsolatedReturnsTrueWithAtomic) {
    unique_ptr<CanonicalQuery> cq = canonicalize("{$atomic: 1, x: 3}");
    ASSERT_TRUE(cq->isIsolated());
}

TEST(CanonicalQueryTest, IsIsolatedReturnsFalseWithIsolated) {
    unique_ptr<CanonicalQuery> cq = canonicalize("{$isolated: 0, x: 3}");
    ASSERT_FALSE(cq->isIsolated());
}

TEST(CanonicalQueryTest, IsIsolatedReturnsFalseWithAtomic) {
    unique_ptr<CanonicalQuery> cq = canonicalize("{$atomic: 0, x: 3}");
    ASSERT_FALSE(cq->isIsolated());
}

/**
 * Test function for CanonicalQuery::normalize.
 */
void testNormalizeQuery(const char* queryStr, const char* expectedExprStr) {
    unique_ptr<CanonicalQuery> cq(canonicalize(queryStr));
    MatchExpression* me = cq->root();
    BSONObj expectedExprObj = fromjson(expectedExprStr);
    unique_ptr<MatchExpression> expectedExpr(parseMatchExpression(expectedExprObj));
    assertEquivalent(queryStr, expectedExpr.get(), me);
}

TEST(CanonicalQueryTest, NormalizeQuerySort) {
    // Field names
    testNormalizeQuery("{b: 1, a: 1}", "{a: 1, b: 1}");
    // Operator types
    testNormalizeQuery("{a: {$gt: 5}, a: {$lt: 10}}}", "{a: {$lt: 10}, a: {$gt: 5}}");
    // Nested queries
    testNormalizeQuery("{a: {$elemMatch: {c: 1, b:1}}}", "{a: {$elemMatch: {b: 1, c:1}}}");
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
    // $in with one argument is rewritten as an equality or regex predicate.
    testNormalizeQuery("{a: {$in: [1]}}", "{a: {$eq: 1}}");
    testNormalizeQuery("{a: {$in: [/./]}}", "{a: {$regex: '.'}}");
    // $in with 0 or more than 1 argument is not modified.
    testNormalizeQuery("{a: {$in: []}}", "{a: {$in: []}}");
    testNormalizeQuery("{a: {$in: [/./, 3]}}", "{a: {$in: [/./, 3]}}");
}

TEST(CanonicalQueryTest, NormalizeWithInPreservesTags) {
    BSONObj obj = fromjson("{x: {$in: [1]}}");
    unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    matchExpression->setTag(new IndexTag(2U, 1U, false));
    matchExpression.reset(CanonicalQuery::normalizeTree(matchExpression.release()));
    IndexTag* tag = dynamic_cast<IndexTag*>(matchExpression->getTag());
    ASSERT(tag);
    ASSERT_EQ(2U, tag->index);
}

TEST(CanonicalQueryTest, NormalizeWithInAndRegexPreservesTags) {
    BSONObj obj = fromjson("{x: {$in: [/a.b/]}}");
    unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    matchExpression->setTag(new IndexTag(2U, 1U, false));
    matchExpression.reset(CanonicalQuery::normalizeTree(matchExpression.release()));
    IndexTag* tag = dynamic_cast<IndexTag*>(matchExpression->getTag());
    ASSERT(tag);
    ASSERT_EQ(2U, tag->index);
}

TEST(CanonicalQueryTest, NormalizeWithInPreservesCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj obj = fromjson("{'': 'string'}");
    auto inMatchExpression = stdx::make_unique<InMatchExpression>();
    inMatchExpression->setCollator(&collator);
    inMatchExpression->addEquality(obj.firstElement());
    unique_ptr<MatchExpression> matchExpression(
        CanonicalQuery::normalizeTree(inMatchExpression.release()));
    ASSERT(matchExpression->matchType() == MatchExpression::MatchType::EQ);
    EqualityMatchExpression* eqMatchExpression =
        static_cast<EqualityMatchExpression*>(matchExpression.get());
    ASSERT_EQ(eqMatchExpression->getCollator(), &collator);
}

TEST(CanonicalQueryTest, CanonicalizeFromBaseQuery) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    const bool isExplain = true;
    const std::string cmdStr =
        "{find:'bogusns', filter:{$or:[{a:1,b:1},{a:1,c:1}]}, projection:{a:1}, sort:{b:1}}";
    auto qr = assertGet(QueryRequest::makeFromFindCommand(nss, fromjson(cmdStr), isExplain));
    auto baseCq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions()));

    MatchExpression* firstClauseExpr = baseCq->root()->getChild(0);
    auto childCq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), *baseCq, firstClauseExpr, ExtensionsCallbackDisallowExtensions()));

    // Descriptive test. The childCq's filter should be the relevant $or clause, rather than the
    // entire query predicate.
    ASSERT_EQ(childCq->getQueryRequest().getFilter(), baseCq->getQueryRequest().getFilter());

    ASSERT_EQ(childCq->getQueryRequest().getProj(), baseCq->getQueryRequest().getProj());
    ASSERT_EQ(childCq->getQueryRequest().getSort(), baseCq->getQueryRequest().getSort());
    ASSERT_TRUE(childCq->getQueryRequest().isExplain());
}

TEST(CanonicalQueryTest, CanonicalQueryFromQRWithNoCollation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    auto cq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
    ASSERT_TRUE(cq->getCollator() == nullptr);
}

TEST(CanonicalQueryTest, CanonicalQueryFromQRWithCollation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setCollation(BSON("locale"
                          << "reverse"));
    auto cq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_TRUE(CollatorInterface::collatorsMatch(cq->getCollator(), &collator));
}

TEST(CanonicalQueryTest, CanonicalQueryFromBaseQueryWithNoCollation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{$or:[{a:1,b:1},{a:1,c:1}]}"));
    auto baseCq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
    MatchExpression* firstClauseExpr = baseCq->root()->getChild(0);
    auto childCq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), *baseCq, firstClauseExpr, ExtensionsCallbackDisallowExtensions()));
    ASSERT_TRUE(baseCq->getCollator() == nullptr);
    ASSERT_TRUE(childCq->getCollator() == nullptr);
}

TEST(CanonicalQueryTest, CanonicalQueryFromBaseQueryWithCollation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{$or:[{a:1,b:1},{a:1,c:1}]}"));
    qr->setCollation(BSON("locale"
                          << "reverse"));
    auto baseCq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
    MatchExpression* firstClauseExpr = baseCq->root()->getChild(0);
    auto childCq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), *baseCq, firstClauseExpr, ExtensionsCallbackDisallowExtensions()));
    ASSERT(baseCq->getCollator());
    ASSERT(childCq->getCollator());
    ASSERT_TRUE(*(childCq->getCollator()) == *(baseCq->getCollator()));
}

TEST(CanonicalQueryTest, SettingCollatorUpdatesCollatorAndMatchExpression) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 'foo', b: {$in: ['bar', 'baz']}}"));
    auto cq = assertGet(CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
    ASSERT_EQUALS(2U, cq->root()->numChildren());
    auto firstChild = cq->root()->getChild(0);
    auto secondChild = cq->root()->getChild(1);
    auto equalityExpr = static_cast<EqualityMatchExpression*>(
        firstChild->matchType() == MatchExpression::EQ ? firstChild : secondChild);
    auto inExpr = static_cast<InMatchExpression*>(
        firstChild->matchType() == MatchExpression::MATCH_IN ? firstChild : secondChild);
    ASSERT_EQUALS(MatchExpression::EQ, equalityExpr->matchType());
    ASSERT_EQUALS(MatchExpression::MATCH_IN, inExpr->matchType());

    ASSERT(!cq->getCollator());
    ASSERT(!equalityExpr->getCollator());
    ASSERT(!inExpr->getCollator());

    unique_ptr<CollatorInterface> collator =
        assertGet(CollatorFactoryInterface::get(txn->getServiceContext())
                      ->makeFromBSON(BSON("locale"
                                          << "reverse")));
    cq->setCollator(std::move(collator));

    ASSERT(cq->getCollator());
    ASSERT_EQUALS(equalityExpr->getCollator(), cq->getCollator());
    ASSERT_EQUALS(inExpr->getCollator(), cq->getCollator());
}

TEST(CanonicalQueryTest, NorWithOneChildNormalizedToNot) {
    unique_ptr<CanonicalQuery> cq(canonicalize("{$nor: [{a: 1}]}"));
    auto root = cq->root();
    ASSERT_EQ(MatchExpression::NOT, root->matchType());
    ASSERT_EQ(1U, root->numChildren());
    ASSERT_EQ(MatchExpression::EQ, root->getChild(0)->matchType());
}

TEST(CanonicalQueryTest, NorWithTwoChildrenNotNormalized) {
    unique_ptr<CanonicalQuery> cq(canonicalize("{$nor: [{a: 1}, {b: 1}]}"));
    auto root = cq->root();
    ASSERT_EQ(MatchExpression::NOR, root->matchType());
}

TEST(CanonicalQueryTest, NorWithOneChildNormalizedAfterNormalizingChild) {
    unique_ptr<CanonicalQuery> cq(canonicalize("{$nor: [{$or: [{a: 1}]}]}"));
    auto root = cq->root();
    ASSERT_EQ(MatchExpression::NOT, root->matchType());
    ASSERT_EQ(1U, root->numChildren());
    ASSERT_EQ(MatchExpression::EQ, root->getChild(0)->matchType());
}

}  // namespace
}  // namespace mongo
