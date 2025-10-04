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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <fmt/format.h>

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using unittest::assertGet;

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");

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
        FAIL(std::string(ss));
    }

    return status.getValue().release();
}

void assertEquivalent(const char* queryStr,
                      const MatchExpression* expected,
                      const MatchExpression* actual,
                      bool skipHashTest = false) {
    str::stream stream;

    const MatchExpressionHasher hash{};
    if (!skipHashTest && hash(expected) != hash(actual)) {
        stream << "MatchExpressions' hashes are not equal.\n";
    }

    if (!expected->equivalent(actual)) {
        stream << "MatchExpressions are not equivalent.\n";
    }

    if (stream.ss.len() > 0) {
        stream << "Original query: " << queryStr << "\nExpected: " << expected->debugString()
               << "\nActual: " << actual->debugString();

        FAIL(std::string(stream));
    }
}

void assertNotEquivalent(const char* queryStr,
                         const MatchExpression* expected,
                         const MatchExpression* actual,
                         bool skipHashTest = false) {
    str::stream stream;

    const MatchExpressionHasher hash{};
    if (!skipHashTest && hash(expected) == hash(actual)) {
        stream << "MatchExpressions' hashes are equal.\n";
    }

    if (expected->equivalent(actual)) {
        stream << "MatchExpressions are equivalent.\n";
    }

    if (stream.ss.len() > 0) {
        stream << "Original query: " << queryStr << "\nExpected: " << expected->debugString()
               << "\nActual: " << actual->debugString();

        FAIL(std::string(stream));
    }
}

TEST(CanonicalQueryTest, IsValidSortKeyMetaProjection) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    // Passing a sortKey meta-projection without a sort is an error.
    {
        auto findCommand = query_request_helper::makeFromFindCommandForTests(
            fromjson("{find: 'testcoll', projection: {foo: {$meta: 'sortKey'}}, '$db': 'test'}"));
        ASSERT_THROWS(
            std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
                .parsedFind = ParsedFindCommandParams{std::move(findCommand)}}),
            DBException);
    }

    // Should be able to successfully create a CQ when there is a sort.
    {
        auto findCommand = query_request_helper::makeFromFindCommandForTests(
            fromjson("{find: 'testcoll', projection: {foo: {$meta: 'sortKey'}}, sort: {bar: 1}, "
                     "'$db': 'test'}"));
        ASSERT_DOES_NOT_THROW(CanonicalQuery(
            {.expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}}));
    }
}

//
// Tests for sortMatchExpressionTree
//

/**
 * Helper function for testing sortMatchExpressionTree().
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
        sortMatchExpressionTree(sortedQueryExprClone.get());
        assertEquivalent(unsortedQueryStr, sortedQueryExpr.get(), sortedQueryExprClone.get());
    }

    // Test that sorting the unsorted expression yields the sorted expression.
    sortMatchExpressionTree(unsortedQueryExpr.get());
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

    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures = allowedFeatures}});
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
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
}

/**
 * Test function for CanonicalQuery::normalize.
 */
void testNormalizeQuery(const char* queryStr,
                        const char* expectedExprStr,
                        bool skipHashTest = false) {
    unique_ptr<CanonicalQuery> cq(canonicalize(queryStr));
    MatchExpression* me = cq->getPrimaryMatchExpression();
    BSONObj expectedExprObj = fromjson(expectedExprStr);
    unique_ptr<MatchExpression> expectedExpr(parseMatchExpression(expectedExprObj));
    assertEquivalent(queryStr, expectedExpr.get(), me, skipHashTest);
}

TEST(CanonicalQueryTest, NormalizeQuerySort) {
    // Field names
    testNormalizeQuery("{b: 1, a: 1}", "{a: 1, b: 1}");
    // Operator types
    testNormalizeQuery("{a: {$gt: 5}, a: {$lt: 10}}", "{a: {$lt: 10}, a: {$gt: 5}}");
    // Nested queries
    testNormalizeQuery("{a: {$elemMatch: {c: 1, b:1}}}", "{a: {$elemMatch: {b: 1, c:1}}}");
}

TEST(CanonicalQueryTest, NormalizeQueryTree) {
    // Single-child $or elimination.
    testNormalizeQuery("{$or: [{b: 1}]}", "{b: 1}");
    // $or deduplication for same-path children.
    testNormalizeQuery("{$or: [{b: 1}, {b: 1}]}", "{b: 1}");
    // Single-child $and elimination.
    testNormalizeQuery("{$or: [{$and: [{a: 1}]}, {b: 1}]}", "{$or: [{a: 1}, {b: 1}]}");
    // Single-child $_internalSchemaXor elimination.
    testNormalizeQuery("{$_internalSchemaXor: [{b: 1}]}", "{b: 1}", /*skipHashTest*/ true);
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
        "1}]}]}",
        /*skipHashTest*/ true);
    // $in with 0 arguments is alwaysFalse
    testNormalizeQuery("{a: {$in: []}}", "{$alwaysFalse: 1}");
    // $in with one argument is rewritten as an equality or regex predicate.
    testNormalizeQuery("{a: {$in: [1]}}", "{a: {$eq: 1}}");
    testNormalizeQuery("{a: {$in: [/./]}}", "{a: {$regex: '.'}}");
    // $in with two or more args is not rewritten
    testNormalizeQuery("{a: {$in: [/foo/, /bar/]}}", "{a: {$in: [/foo/, /bar/]}}");
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
        "{$_internalSchemaCond: [{a: 1}, {b: 1}, {c: 1}]}",
        /*skipHashTest*/ true);
}

TEST(CanonicalQueryTest, CanonicalizeFromBaseQuery) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    const std::string cmdStr =
        "{find:'bogusns', filter:{$or:[{a:1,b:1},{a:1,c:1}]}, projection:{a:1}, sort:{b:1}, '$db': "
        "'test'}";
    auto findCommand = query_request_helper::makeFromFindCommandForTests(fromjson(cmdStr));
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx.get(), *findCommand)
                      .explain(explain::VerbosityEnum::kQueryPlanner)
                      .build();
    auto baseCq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = std::move(expCtx),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)},
    });

    MatchExpression* firstClauseExpr = baseCq->getPrimaryMatchExpression()->getChild(0);
    auto childCq = std::make_unique<CanonicalQuery>(opCtx.get(), *baseCq, 0);

    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getFilter(), firstClauseExpr->serialize());

    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getProjection(),
                      baseCq->getFindCommandRequest().getProjection());
    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getSort(),
                      baseCq->getFindCommandRequest().getSort());
    ASSERT_TRUE(childCq->getExplain());
}

TEST(CanonicalQueryTest, CanonicalizeFromBaseQueryWithSpecialFeature) {
    // Like the above test, but use $text which is a 'special feature' not always allowed. This is
    // meant to reproduce SERVER-XYZ.
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    const std::string cmdStr = R"({
        find:'bogusns',
        filter: {
            $or:[
                {a: 'foo'},
                {$text: {$search: 'bar'}}
            ]
        },
        projection: {a:1},
        sort: {b:1},
        $db: 'test'
    })";
    auto findCommand = query_request_helper::makeFromFindCommandForTests(fromjson(cmdStr));
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx.get(), *findCommand)
                      .explain(explain::VerbosityEnum::kQueryPlanner)
                      .build();
    auto baseCq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = std::move(expCtx),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures =
                                                  MatchExpressionParser::kAllowAllSpecialFeatures},
    });

    // Note: be sure to use the second child to get $text, since we 'normalize' and sort the
    // MatchExpression tree as part of canonicalization. This will put the text search clause
    // second.
    MatchExpression* secondClauseExpr = baseCq->getPrimaryMatchExpression()->getChild(1);
    auto childCq = std::make_unique<CanonicalQuery>(opCtx.get(), *baseCq, 1);

    ASSERT_BSONOBJ_EQ(childCq->getFindCommandRequest().getFilter(), secondClauseExpr->serialize());

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
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    ASSERT_TRUE(cq->getCollator() == nullptr);
}

TEST(CanonicalQueryTest, CanonicalQueryFromQRWithCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setCollation(BSON("locale" << "reverse"));
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_TRUE(CollatorInterface::collatorsMatch(cq->getCollator(), &collator));
}

TEST(CanonicalQueryTest, CanonicalQueryFromBaseQueryWithNoCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{$or:[{a:1,b:1},{a:1,c:1}]}"));
    auto baseCq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    auto childCq = std::make_unique<CanonicalQuery>(opCtx.get(), *baseCq, 0);
    ASSERT_TRUE(baseCq->getCollator() == nullptr);
    ASSERT_TRUE(childCq->getCollator() == nullptr);
}

TEST(CanonicalQueryTest, CanonicalQueryFromBaseQueryWithCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{$or:[{a:1,b:1},{a:1,c:1}]}"));
    findCommand->setCollation(BSON("locale" << "reverse"));
    auto baseCq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    auto childCq = std::make_unique<CanonicalQuery>(opCtx.get(), *baseCq, 0);
    ASSERT(baseCq->getCollator());
    ASSERT(childCq->getCollator());
    ASSERT_TRUE(*(childCq->getCollator()) == *(baseCq->getCollator()));
}

TEST(CanonicalQueryTest, SettingCollatorUpdatesCollatorAndMatchExpression) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{a: 'foo', b: {$in: ['bar', 'baz']}}"));
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    ASSERT_EQUALS(2U, cq->getPrimaryMatchExpression()->numChildren());
    auto firstChild = cq->getPrimaryMatchExpression()->getChild(0);
    auto secondChild = cq->getPrimaryMatchExpression()->getChild(1);
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
                      ->makeFromBSON(BSON("locale" << "reverse")));
    cq->setCollator(std::move(collator));

    ASSERT(cq->getCollator());
    ASSERT_EQUALS(equalityExpr->getCollator(), cq->getCollator());
    ASSERT_EQUALS(inExpr->getCollator(), cq->getCollator());
}

TEST(CanonicalQueryTest, NorWithOneChildNormalizedToNot) {
    unique_ptr<CanonicalQuery> cq(canonicalize("{$nor: [{a: 1}]}"));
    auto root = cq->getPrimaryMatchExpression();
    ASSERT_EQ(MatchExpression::NOT, root->matchType());
    ASSERT_EQ(1U, root->numChildren());
    ASSERT_EQ(MatchExpression::EQ, root->getChild(0)->matchType());
}

TEST(CanonicalQueryTest, NorWithTwoChildrenNotNormalized) {
    RAIIServerParameterControllerForTest controller(
        "internalQueryEnableBooleanExpressionsSimplifier", false);

    unique_ptr<CanonicalQuery> cq(canonicalize("{$nor: [{a: 1}, {b: 1}]}"));
    auto root = cq->getPrimaryMatchExpression();
    ASSERT_EQ(MatchExpression::NOR, root->matchType());
}

TEST(CanonicalQueryTest, NorWithOneChildNormalizedAfterNormalizingChild) {
    unique_ptr<CanonicalQuery> cq(canonicalize("{$nor: [{$or: [{a: 1}]}]}"));
    auto root = cq->getPrimaryMatchExpression();
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
    ASSERT_DOES_NOT_THROW(CanonicalQuery(
        {.expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
         .parsedFind = ParsedFindCommandParams{
             .findCommand = std::move(findCommand),
             .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}}));
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
    ASSERT_THROWS(
        std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}}),
        DBException);
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
    // We never parameterize unless SBE is fully enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);

    auto cq =
        canonicalize("{$text: {$search: \"Hello World!\"}}",
                     MatchExpressionParser::kDefaultSpecialFeatures | MatchExpressionParser::kText);
    ASSERT_FALSE(cq->isParameterized());
}

TEST(CanonicalQueryTest, DoParameterizeRegularExpressions) {
    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);

    auto cq = canonicalize("{a: 1, b: {$lt: 5}}");
    ASSERT_TRUE(cq->isParameterized());
}
}  // namespace
}  // namespace mongo
