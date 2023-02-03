/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
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
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj,
                                     std::move(expCtx),
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!status.isOK()) {
        str::stream ss;
        ss << "failed to parse query: " << obj.toString()
           << ". Reason: " << status.getStatus().toString();
        FAIL(ss);
    }

    return status.getValue().release();
}

void assertEquivalent(const char* queryStr,
                      const MatchExpression* expected,
                      const MatchExpression* actual) {
    if (actual->equivalent(expected)) {
        return;
    }
    str::stream ss;
    ss << "Match expressions are not equivalent."
       << "\nOriginal query: " << queryStr << "\nExpected: " << expected->debugString()
       << "\nActual: " << actual->debugString();
    FAIL(ss);
}

void assertNotEquivalent(const char* queryStr,
                         const MatchExpression* expected,
                         const MatchExpression* actual) {
    if (!actual->equivalent(expected)) {
        return;
    }
    str::stream ss;
    ss << "Match expressions are equivalent."
       << "\nOriginal query: " << queryStr << "\nExpected: " << expected->debugString()
       << "\nActual: " << actual->debugString();
    FAIL(ss);
}

TEST(CanonicalQueryTest, IsValidSortKeyMetaProjection) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    // Passing a sortKey meta-projection without a sort is an error.
    {
        auto findCommand = query_request_helper::makeFromFindCommandForTests(
            fromjson("{find: 'testcoll', projection: {foo: {$meta: 'sortKey'}}, '$db': 'test'}"));
        auto cq = CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand));
        ASSERT_NOT_OK(cq.getStatus());
    }

    // Should be able to successfully create a CQ when there is a sort.
    {
        auto findCommand = query_request_helper::makeFromFindCommandForTests(
            fromjson("{find: 'testcoll', projection: {foo: {$meta: 'sortKey'}}, sort: {bar: 1}, "
                     "'$db': 'test'}"));
        auto cq = CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand));
        ASSERT_OK(cq.getStatus());
    }
}

//
// Tests for MatchExpression::sortTree
//

/**
 * Helper function for testing MatchExpression::sortTree().
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
        MatchExpression::sortTree(sortedQueryExprClone.get());
        assertEquivalent(unsortedQueryStr, sortedQueryExpr.get(), sortedQueryExprClone.get());
    }

    // Test that sorting the unsorted expression yields the sorted expression.
    MatchExpression::sortTree(unsortedQueryExpr.get());
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
unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
                                            MatchExpressionParser::kDefaultSpecialFeatures) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));

    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(),
                                                     std::move(findCommand),
                                                     false,
                                                     nullptr,
                                                     ExtensionsCallbackNoop(),
                                                     allowedFeatures);

    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                             const char* sortStr,
                                             const char* projStr) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    findCommand->setSort(fromjson(sortStr));
    findCommand->setProjection(fromjson(projStr));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand));
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
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
    // Single-child $_internalSchemaXor elimination.
    testNormalizeQuery("{$_internalSchemaXor: [{b: 1}]}", "{b: 1}");
    // $or absorbs $or children.
    testNormalizeQuery("{$or: [{a: 1}, {$or: [{b: 1}, {$or: [{c: 1}]}]}, {d: 1}]}",
                       "{$or: [{a: 1}, {b: 1}, {c: 1}, {d: 1}]}");
    // $and absorbs $and children.
    testNormalizeQuery("{$and: [{$and: [{a: 1}, {b: 1}]}, {c: 1}]}",
                       "{$and: [{a: 1}, {b: 1}, {c: 1}]}");
    // $_internalSchemaXor _does not_ absorb any children.
    testNormalizeQuery(
        "{$_internalSchemaXor: [{$and: [{a: 1}, {b:1}]}, {$_internalSchemaXor: [{c: 1}, {d: 1}]}]}",
        "{$_internalSchemaXor: [{$and: [{a: 1}, {b:1}]}, {$_internalSchemaXor: [{c: 1}, {d: "
        "1}]}]}");
    // $in with one argument is rewritten as an equality or regex predicate.
    testNormalizeQuery("{a: {$in: [1]}}", "{a: {$eq: 1}}");
    testNormalizeQuery("{a: {$in: [/./]}}", "{a: {$regex: '.'}}");
    // $in with 0 or more than 1 argument is not modified.
    testNormalizeQuery("{a: {$in: []}}", "{a: {$in: []}}");
    testNormalizeQuery("{a: {$in: [/./, 3]}}", "{a: {$in: [/./, 3]}}");
    // Child of $elemMatch object expression is normalized.
    testNormalizeQuery("{a: {$elemMatch: {$or: [{b: 1}]}}}", "{a: {$elemMatch: {b: 1}}}");

    // All three children of $_internalSchemaCond are normalized.
    testNormalizeQuery(
        "{$_internalSchemaCond: ["
        "  {$and: [{a: 1}]},"
        "  {$or: [{b: 1}]},"
        "  {$_internalSchemaXor: [{c: 1}]}"
        "]}",
        "{$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}");
}

TEST(CanonicalQueryTest, CanonicalizeFromBaseQuery) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    const bool isExplain = true;
    const std::string cmdStr =
        "{find:'bogusns', filter:{$or:[{a:1,b:1},{a:1,c:1}]}, projection:{a:1}, sort:{b:1}, '$db': "
        "'test'}";
    auto findCommand = query_request_helper::makeFromFindCommandForTests(fromjson(cmdStr));
    auto baseCq =
        assertGet(CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand), isExplain));

    MatchExpression* firstClauseExpr = baseCq->root()->getChild(0);
    auto childCq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), *baseCq, firstClauseExpr));

    BSONObjBuilder expectedFilter;
    firstClauseExpr->serialize(&expectedFilter);
    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getFilter(), expectedFilter.obj());

    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getProjection(),
                      baseCq->getFindCommandRequest().getProjection());
    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getSort(),
                      baseCq->getFindCommandRequest().getSort());
    ASSERT_TRUE(childCq->getExplain());
}

TEST(CanonicalQueryTest, CanonicalQueryFromQRWithNoCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    auto cq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand)));
    ASSERT_TRUE(cq->getCollator() == nullptr);
}

TEST(CanonicalQueryTest, CanonicalQueryFromQRWithCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setCollation(BSON("locale"
                                   << "reverse"));
    auto cq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand)));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_TRUE(CollatorInterface::collatorsMatch(cq->getCollator(), &collator));
}

TEST(CanonicalQueryTest, CanonicalQueryFromBaseQueryWithNoCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{$or:[{a:1,b:1},{a:1,c:1}]}"));
    auto baseCq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand)));
    MatchExpression* firstClauseExpr = baseCq->root()->getChild(0);
    auto childCq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), *baseCq, firstClauseExpr));
    ASSERT_TRUE(baseCq->getCollator() == nullptr);
    ASSERT_TRUE(childCq->getCollator() == nullptr);
}

TEST(CanonicalQueryTest, CanonicalQueryFromBaseQueryWithCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{$or:[{a:1,b:1},{a:1,c:1}]}"));
    findCommand->setCollation(BSON("locale"
                                   << "reverse"));
    auto baseCq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand)));
    MatchExpression* firstClauseExpr = baseCq->root()->getChild(0);
    auto childCq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), *baseCq, firstClauseExpr));
    ASSERT(baseCq->getCollator());
    ASSERT(childCq->getCollator());
    ASSERT_TRUE(*(childCq->getCollator()) == *(baseCq->getCollator()));
}

TEST(CanonicalQueryTest, SettingCollatorUpdatesCollatorAndMatchExpression) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{a: 'foo', b: {$in: ['bar', 'baz']}}"));
    auto cq = assertGet(CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand)));
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
        assertGet(CollatorFactoryInterface::get(opCtx->getServiceContext())
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

void assertValidSortOrder(BSONObj sort, BSONObj filter = BSONObj{}) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(filter);
    findCommand->setSort(sort);
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(findCommand),
                                     false,
                                     nullptr,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
}

TEST(CanonicalQueryTest, ValidSortOrdersCanonicalizeSuccessfully) {
    assertValidSortOrder(fromjson("{}"));
    assertValidSortOrder(fromjson("{a: 1}"));
    assertValidSortOrder(fromjson("{a: -1}"));
    assertValidSortOrder(fromjson("{a: {$meta: \"textScore\"}}"),
                         fromjson("{$text: {$search: 'keyword'}}"));
    assertValidSortOrder(fromjson("{a: {$meta: \"randVal\"}}"));
}

void assertInvalidSortOrder(BSONObj sort) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSort(sort);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand));
    ASSERT_NOT_OK(statusWithCQ.getStatus());
}

TEST(CanonicalQueryTest, InvalidSortOrdersFailToCanonicalize) {
    assertInvalidSortOrder(fromjson("{a: 100}"));
    assertInvalidSortOrder(fromjson("{a: 0}"));
    assertInvalidSortOrder(fromjson("{a: -100}"));
    assertInvalidSortOrder(fromjson("{a: Infinity}"));
    assertInvalidSortOrder(fromjson("{a: -Infinity}"));
    assertInvalidSortOrder(fromjson("{a: true}"));
    assertInvalidSortOrder(fromjson("{a: false}"));
    assertInvalidSortOrder(fromjson("{a: null}"));
    assertInvalidSortOrder(fromjson("{a: {}}"));
    assertInvalidSortOrder(fromjson("{a: {b: 1}}"));
    assertInvalidSortOrder(fromjson("{a: []}"));
    assertInvalidSortOrder(fromjson("{a: [1, 2, 3]}"));
    assertInvalidSortOrder(fromjson("{a: \"\"}"));
    assertInvalidSortOrder(fromjson("{a: \"bb\"}"));
    assertInvalidSortOrder(fromjson("{a: {$meta: 1}}"));
    assertInvalidSortOrder(fromjson("{a: {$meta: \"image\"}}"));
    assertInvalidSortOrder(fromjson("{a: {$world: \"textScore\"}}"));
    assertInvalidSortOrder(fromjson("{a: {$meta: \"textScore\", b: 1}}"));
    assertInvalidSortOrder(fromjson("{'': 1}"));
    assertInvalidSortOrder(fromjson("{'': -1}"));
}

TEST(CanonicalQueryTest, DoNotParameterizeTextExpressions) {
    RAIIServerParameterControllerForTest controllerSBEPlanCache("featureFlagSbeFull", true);
    auto cq =
        canonicalize("{$text: {$search: \"Hello World!\"}}",
                     MatchExpressionParser::kDefaultSpecialFeatures | MatchExpressionParser::kText);
    ASSERT_FALSE(cq->isParameterized());
}

TEST(CanonicalQueryTest, DoParameterizeRegularExpressions) {
    RAIIServerParameterControllerForTest controllerSBEPlanCache("featureFlagSbeFull", true);
    auto cq = canonicalize("{a: 1, b: {$lt: 5}}");
    ASSERT_TRUE(cq->isParameterized());
}
}  // namespace
}  // namespace mongo
