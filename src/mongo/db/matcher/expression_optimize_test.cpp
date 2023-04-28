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

#include "mongo/db/pipeline/expression.h"

#include <vector>

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");

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
        str::stream ss;
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
Status isValid(const std::string& queryStr, const FindCommandRequest& findCommand) {
    BSONObj queryObj = fromjson(queryStr);
    std::unique_ptr<MatchExpression> me(parseMatchExpression(queryObj));
    me = MatchExpression::optimize(std::move(me));
    if (auto status = CanonicalQuery::isValid(me.get(), findCommand).getStatus(); !status.isOK()) {
        return status;
    }
    return CanonicalQuery::isValidNormalized(me.get());
}

TEST(ExpressionOptimizeTest, IsValidText) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Valid: regular TEXT.
    ASSERT_OK(isValid("{$text: {$search: 's'}}", *findCommand));

    // Valid: TEXT inside OR.
    ASSERT_OK(
        isValid("{$or: ["
                "    {$text: {$search: 's'}},"
                "    {a: 1}"
                "]}",
                *findCommand));

    // Valid: TEXT outside NOR.
    ASSERT_OK(isValid("{$text: {$search: 's'}, $nor: [{a: 1}, {b: 1}]}", *findCommand));

    // Invalid: TEXT inside NOR.
    ASSERT_NOT_OK(isValid("{$nor: [{$text: {$search: 's'}}, {a: 1}]}", *findCommand));

    // Invalid: TEXT inside NOR.
    ASSERT_NOT_OK(
        isValid("{$nor: ["
                "    {$or: ["
                "        {$text: {$search: 's'}},"
                "        {a: 1}"
                "    ]},"
                "    {a: 2}"
                "]}",
                *findCommand));

    // Invalid: >1 TEXT.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {$text: {$search: 's'}},"
                "    {$text: {$search: 't'}}"
                "]}",
                *findCommand));

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
                *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidTextTailable) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    query_request_helper::setTailableMode(TailableModeEnum::kTailable, findCommand.get());
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: TEXT and tailable.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidGeo) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Valid: regular GEO_NEAR.
    ASSERT_OK(isValid("{a: {$near: [0, 0]}}", *findCommand));

    // Valid: GEO_NEAR inside nested AND.
    ASSERT_OK(
        isValid("{$and: ["
                "    {$and: ["
                "        {a: {$near: [0, 0]}},"
                "        {b: 1}"
                "    ]},"
                "    {c: 1}"
                "]}",
                *findCommand));

    // Invalid: >1 GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {a: {$near: [0, 0]}},"
                "    {b: {$near: [0, 0]}}"
                "]}",
                *findCommand));

    // Invalid: >1 GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$and: ["
                "    {a: {$geoNear: [0, 0]}},"
                "    {b: {$near: [0, 0]}}"
                "]}",
                *findCommand));

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
                *findCommand));

    // Invalid: GEO_NEAR inside NOR.
    ASSERT_NOT_OK(
        isValid("{$nor: ["
                "    {a: {$near: [0, 0]}},"
                "    {b: 1}"
                "]}",
                *findCommand));

    // Invalid: GEO_NEAR inside OR.
    ASSERT_NOT_OK(
        isValid("{$or: ["
                "    {a: {$near: [0, 0]}},"
                "    {b: 1}"
                "]}",
                *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidTextAndGeo) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: TEXT and GEO_NEAR.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}, a: {$near: [0, 0]}}", *findCommand));

    // Invalid: TEXT and GEO_NEAR.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}, a: {$geoNear: [0, 0]}}", *findCommand));

    // Invalid: TEXT and GEO_NEAR.
    ASSERT_NOT_OK(
        isValid("{$or: ["
                "    {$text: {$search: 's'}},"
                "    {a: 1}"
                " ],"
                " b: {$near: [0, 0]}}",
                *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidTextAndNaturalAscending) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: TEXT and {$natural: 1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidTextAndNaturalDescending) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSort(fromjson("{$natural: -1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: TEXT and {$natural: -1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidTextAndHint) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setHint(fromjson("{a: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: TEXT and {$natural: -1} sort order.
    ASSERT_NOT_OK(isValid("{$text: {$search: 's'}}", *findCommand));
}

// SERVER-14366
TEST(ExpressionOptimizeTest, IsValidGeoNearNaturalSort) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSort(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: GEO_NEAR and {$natural: 1} sort order.
    ASSERT_NOT_OK(
        isValid("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}", *findCommand));
}

// SERVER-14366
TEST(ExpressionOptimizeTest, IsValidGeoNearNaturalHint) {
    // Filter inside FindCommandRequest is not used.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setHint(fromjson("{$natural: 1}"));
    ASSERT_OK(query_request_helper::validateFindCommandRequest(*findCommand));

    // Invalid: GEO_NEAR and {$natural: 1} hint.
    ASSERT_NOT_OK(
        isValid("{a: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}", *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidNaturalSortIndexHint) {
    auto findCommand = query_request_helper::makeFromFindCommandForTests(
        fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {a: 1}, '$db': 'test'}"));

    // Invalid: {$natural: 1} sort order and index hint.
    ASSERT_NOT_OK(isValid("{}", *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidNaturalSortNaturalHint) {
    auto findCommand = query_request_helper::makeFromFindCommandForTests(
        fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {$natural: 1}, '$db': 'test'}"));

    // Valid: {$natural: 1} sort order and {$natural: 1} hint.
    ASSERT_OK(isValid("{}", *findCommand));
}

TEST(ExpressionOptimizeTest, IsValidNaturalSortNaturalHintDifferentDirections) {
    auto findCommand = query_request_helper::makeFromFindCommandForTests(
        fromjson("{find: 'testcoll', sort: {$natural: 1}, hint: {$natural: -1}, '$db': 'test'}"));

    // Invalid: {$natural: 1} sort order and {$natural: -1} hint.
    ASSERT_NOT_OK(isValid("{}", *findCommand));
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
    auto inMatchExpression = std::make_unique<InMatchExpression>(""_sd);
    inMatchExpression->setCollator(&collator);
    std::vector<BSONElement> equalities{obj.firstElement()};
    ASSERT_OK(inMatchExpression->setEqualities(std::move(equalities)));
    auto matchExpression = MatchExpression::optimize(std::move(inMatchExpression));
    ASSERT(matchExpression->matchType() == MatchExpression::MatchType::EQ);
    EqualityMatchExpression* eqMatchExpression =
        static_cast<EqualityMatchExpression*>(matchExpression.get());
    ASSERT_EQ(eqMatchExpression->getCollator(), &collator);
}

TEST(ExpressionOptimizeTest, AndWithAlwaysFalseChildOptimizesToAlwaysFalse) {
    BSONObj obj = fromjson("{$and: [{a: 1}, {$alwaysFalse: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{$alwaysFalse: 1}"));
}

TEST(ExpressionOptimizeTest, AndRemovesAlwaysTrueChildren) {
    BSONObj obj = fromjson("{$and: [{a: 1}, {$alwaysTrue: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{a: {$eq: 1}}"));
}

TEST(ExpressionOptimizeTest, AndWithSingleChildAlwaysTrueOptimizesToEmptyAnd) {
    BSONObj obj = fromjson("{$and: [{$alwaysTrue: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    // TODO SERVER-34759 We want this to optimize to an AlwaysTrueMatchExpression.
    ASSERT_TRUE(dynamic_cast<AndMatchExpression*>(optimizedMatchExpression.get()));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{}"));
}

TEST(ExpressionOptimizeTest, AndWithEachChildAlwaysTrueOptimizesToEmptyAnd) {
    BSONObj obj = fromjson("{$and: [{$alwaysTrue: 1}, {$alwaysTrue: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    // TODO SERVER-34759 We want this to optimize to an AlwaysTrueMatchExpression.
    ASSERT_TRUE(dynamic_cast<AndMatchExpression*>(optimizedMatchExpression.get()));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{}"));
}

TEST(ExpressionOptimizeTest, NestedAndWithAlwaysFalseOptimizesToAlwaysFalse) {
    BSONObj obj = fromjson("{$and: [{$and: [{$alwaysFalse: 1}, {a: 1}]}, {b: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{$alwaysFalse: 1}"));
}

TEST(ExpressionOptimizeTest, OrWithAlwaysTrueOptimizesToAlwaysTrue) {
    BSONObj obj = fromjson("{$or: [{a: 1}, {$alwaysTrue: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{}"));
}

TEST(ExpressionOptimizeTest, OrRemovesAlwaysFalseChildren) {
    BSONObj obj = fromjson("{$or: [{a: 1}, {$alwaysFalse: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{a: {$eq: 1}}"));
}

TEST(ExpressionOptimizeTest, OrPromotesSingleAlwaysFalseAfterOptimize) {
    // The nested predicate is always false. This test is designed to reproduce SERVER-34714.
    BSONObj obj = fromjson("{$or: [{a: {$all: []}}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_TRUE(dynamic_cast<AlwaysFalseMatchExpression*>(optimizedMatchExpression.get()));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{$alwaysFalse: 1}"));
}

TEST(ExpressionOptimizeTest, OrPromotesSingleAlwaysFalse) {
    BSONObj obj = fromjson("{$or: [{$alwaysFalse: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_TRUE(dynamic_cast<AlwaysFalseMatchExpression*>(optimizedMatchExpression.get()));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{$alwaysFalse: 1}"));
}

TEST(ExpressionOptimizeTest, OrPromotesMultipleAlwaysFalse) {
    BSONObj obj = fromjson("{$or: [{$alwaysFalse: 1}, {a: {$all: []}}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_TRUE(dynamic_cast<AlwaysFalseMatchExpression*>(optimizedMatchExpression.get()));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{$alwaysFalse: 1}"));
}

TEST(ExpressionOptimizeTest, NestedOrWithAlwaysTrueOptimizesToAlwaysTrue) {
    BSONObj obj = fromjson("{$or: [{$or: [{$alwaysTrue: 1}, {a: 1}]}, {b: 1}]}");
    std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
    auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
    ASSERT_BSONOBJ_EQ(optimizedMatchExpression->serialize(), fromjson("{}"));
}

TEST(ExpressionOptimizeTest, OrRewrittenToIn) {
    const std::vector<std::pair<std::string, std::string>> queries = {
        {"{$or: [{f1: 5}, {f1: 3}, {f1: 7}]}", "{ f1: { $in: [ 3, 5, 7 ] } }"},
        {"{$or: [{f1: {$eq: 5}}, {f1: {$eq: 3}}, {f1: {$eq: 7}}]}", "{ f1: { $in: [ 3, 5, 7 ] } }"},
        {"{$or: [{f1: 42}, {f1: NaN}, {f1: 99}]}", "{ f1: { $in: [ NaN, 42, 99 ] } }"},
        {"{$or: [{f1: /^x/}, {f1:'ab'}]}", "{ f1: { $in: [ \"ab\", /^x/ ] } }"},
        {"{$or: [{f1: /^x/}, {f1:'^a'}]}", "{ f1: { $in: [ \"^a\", /^x/ ] } }"},
        {"{$or: [{f1: 42}, {f1: null}, {f1: 99}]}", "{ f1: { $in: [ null, 42, 99 ] } }"},
        {"{$or: [{f1: 1}, {f2: 9}, {f1: 99}]}",
         "{ $or: [ { f1: { $in: [ 1, 99 ] } }, { f2: { $eq: 9 } } ] }"},
        {"{$and: [{$or: [{f1: 7}, {f1: 3}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]}",
         "{ $and: [ { f1: { $in: [ 3, 5, 7 ] } }, { f1: { $in: [ 1, 2, 3 ] } } ] }"},
        {"{$or: [{$or: [{f1: 7}, {f1: 3}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]}",
         "{ $or: [ { f1: { $in: [ 3, 5, 7 ] } }, { f1: { $in: [ 1, 2, 3 ] } } ] }"},
        {"{$or: [{$and: [{f1: 7}, {f2: 7}, {f1: 5}]}, {$or: [{f1: 1}, {f1: 2}, {f1: 3}]}]}",
         "{ $or: [ { $and: [ { f1: { $eq: 7 } }, { f2: { $eq: 7 } }, { f1: { $eq: 5 } } ] },"
         " { f1: { $in: [ 1, 2, 3 ] } } ] }"},
        {"{$or: [{$and: [ { f1: null } ] } ] }", "{ f1: { $eq: null } }"},
    };

    auto optimizeExpr = [](std::string exprStr) {
        auto obj = fromjson(exprStr);
        std::unique_ptr<MatchExpression> matchExpression(parseMatchExpression(obj));
        auto optimizedMatchExpression = MatchExpression::optimize(std::move(matchExpression));
        return optimizedMatchExpression->serialize();
    };

    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[0].first), fromjson(queries[0].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[1].first), fromjson(queries[1].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[2].first), fromjson(queries[2].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[3].first), fromjson(queries[3].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[4].first), fromjson(queries[4].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[5].first), fromjson(queries[5].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[6].first), fromjson(queries[6].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[7].first), fromjson(queries[7].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[8].first), fromjson(queries[8].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[9].first), fromjson(queries[9].second));
    ASSERT_BSONOBJ_EQ(optimizeExpr(queries[10].first), fromjson(queries[10].second));
}

}  // namespace
}  // namespace mongo
