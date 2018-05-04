/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/db/pipeline/expression.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const NamespaceString nss("testdb.testcoll");

using unittest::assertGet;

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
    std::unique_ptr<MatchExpression> me(parseMatchExpression(queryObj));
    me = MatchExpression::optimize(std::move(me));
    return CanonicalQuery::isValid(me.get(), qrRaw);
}

TEST(ExpressionOptimizeTest, IsValidText) {
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

TEST(ExpressionOptimizeTest, IsValidTextTailable) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setTailableMode(TailableMode::kTailable);
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and tailable.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidGeo) {
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

TEST(ExpressionOptimizeTest, IsValidTextAndGeo) {
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

TEST(ExpressionOptimizeTest, IsValidTextAndNaturalAscending) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and {$natural: 1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidTextAndNaturalDescending) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(fromjson("{$natural: -1}"));
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and {$natural: -1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidTextAndHint) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setHint(fromjson("{a: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and {$natural: -1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

// SERVER-14366
TEST(ExpressionOptimizeTest, IsValidGeoNearNaturalSort) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: GEO_NEAR and {$natural: 1} sort order.
    ASSERT_NOT_OK(isValid("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}", *qr));
}

// SERVER-14366
TEST(ExpressionOptimizeTest, IsValidGeoNearNaturalHint) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(qr->validate());

    // Invalid: GEO_NEAR and {$natural: 1} hint.
    ASSERT_NOT_OK(isValid("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidTextAndSnapshot) {
    // Filter inside QueryRequest is not used.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setSnapshot(true);
    ASSERT_OK(qr->validate());

    // Invalid: TEXT and snapshot.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidNaturalSortIndexHint) {
    const bool isExplain = false;
    auto qr = assertGet(QueryRequest::makeFromFindCommand(
        nss, fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {a: 1}}"), isExplain));

    // Invalid: {$natural: 1} sort order and index hint.
    ASSERT_NOT_OK(isValid("{}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidNaturalSortNaturalHint) {
    const bool isExplain = false;
    auto qr = assertGet(QueryRequest::makeFromFindCommand(
        nss, fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {$natural: 1}}"), isExplain));

    // Valid: {$natural: 1} sort order and {$natural: 1} hint.
    ASSERT_OK(isValid("{}", *qr));
}

TEST(ExpressionOptimizeTest, IsValidNaturalSortNaturalHintDifferentDirections) {
    const bool isExplain = false;
    auto qr = assertGet(QueryRequest::makeFromFindCommand(
        nss, fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {$natural: -1}}"), isExplain));

    // Invalid: {$natural: 1} sort order and {$natural: -1} hint.
    ASSERT_NOT_OK(isValid("{}", *qr));
}

TEST(ExpressionOptimizeTest, NormalizeWithInPreservesTags) {
    BSONObj obj = fromjson("{x: {$in: [1]}}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    matchExpression->setTag(new IndexTag(2U, 1U, false));
    matchExpression = MatchExpression::optimize(std::move(matchExpression));
    IndexTag* tag = dynamic_cast<IndexTag*>(matchExpression->getTag());
    ASSERT(tag);
    ASSERT_EQ(2U, tag->index);
}

TEST(ExpressionOptimizeTest, NormalizeWithInAndRegexPreservesTags) {
    BSONObj obj = fromjson("{x: {$in: [/a.b/]}}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    matchExpression->setTag(new IndexTag(2U, 1U, false));
    matchExpression = MatchExpression::optimize(std::move(matchExpression));
    IndexTag* tag = dynamic_cast<IndexTag*>(matchExpression->getTag());
    ASSERT(tag);
    ASSERT_EQ(2U, tag->index);
}

TEST(ExpressionOptimizeTest, NormalizeWithInPreservesCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj obj = fromjson("{'': 'string'}");
    auto inMatchExpression = stdx::make_unique<InMatchExpression>();
    inMatchExpression->setCollator(&collator);
    std::vector<BSONElement> equalities{obj.firstElement()};
    ASSERT_OK(inMatchExpression->setEqualities(std::move(equalities)));
    auto matchExpression = MatchExpression::optimize(std::move(inMatchExpression));
    ASSERT(matchExpression->matchType() == MatchExpression::MatchType::EQ);
    EqualityMatchExpression* eqMatchExpression =
        static_cast<EqualityMatchExpression*>(matchExpression.get());
    ASSERT_EQ(eqMatchExpression->getCollator(), &collator);
}

TEST(ExpressionOptimizeTest, OrPromotesSingleAlwaysFalseAfterOptimize) {
    // The nested predicate is always false. This test is designed to reproduce SERVER-34714.
    BSONObj obj = fromjson("{$or: [{a: {$all: []}}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    BSONObjBuilder bob;
    optimizedMatchExpression->serialize(&bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), fromjson("{$alwaysFalse: 1}"));
}

TEST(ExpressionOptimizeTest, OrPromotesSingleAlwaysFalse) {
    BSONObj obj = fromjson("{$or: [{$alwaysFalse: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    BSONObjBuilder bob;
    optimizedMatchExpression->serialize(&bob);
    ASSERT_BSONOBJ_EQ(bob.obj(), fromjson("{$alwaysFalse: 1}"));
}

}  // namespace
}  // namespace mongo
