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
#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(InternalSchemaObjectMatchExpression, RejectsNonObjectElements) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b" << 1), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << 1)));
    ASSERT_FALSE(objMatch.matchesBSON(BSON("a"
                                           << "string")));
    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << BSON_ARRAY(BSONNULL))));
}

TEST(InternalSchemaObjectMatchExpression, RejectsObjectsThatDontMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b" << BSON("$type"
                                                                 << "string")),
                                                expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << BSON("b" << 1))));
    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << BSON("b" << BSONObj()))));
}

TEST(InternalSchemaObjectMatchExpression, AcceptsObjectsThatMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b" << BSON("$type"
                                                                 << "string")),
                                                expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_TRUE(objMatch.matchesBSON(BSON("a" << BSON("b"
                                                      << "string"))));
    ASSERT_TRUE(objMatch.matchesBSON(BSON("a" << BSON("b"
                                                      << "string"
                                                      << "c" << 1))));
    ASSERT_FALSE(
        objMatch.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b"
                                                                           << "string")))));
    ASSERT_TRUE(objMatch.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY("string")))));
}

TEST(InternalSchemaObjectMatchExpression, DottedPathAcceptsObjectsThatMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSON("b.c.d" << BSON("$type"
                                                                     << "string")),
                                                expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << BSON("d"
                                                       << "string"))));
    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1))))));
    ASSERT_TRUE(objMatch.matchesBSON(BSON("a" << BSON("b" << BSON("c" << BSON("d"
                                                                              << "foo"))))));
}

TEST(InternalSchemaObjectMatchExpression, EmptyMatchAcceptsAllObjects) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto subExpr = MatchExpressionParser::parse(BSONObj(), expCtx);
    ASSERT_OK(subExpr.getStatus());

    InternalSchemaObjectMatchExpression objMatch("a"_sd, std::move(subExpr.getValue()));

    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << 1)));
    ASSERT_FALSE(objMatch.matchesBSON(BSON("a"
                                           << "string")));
    ASSERT_TRUE(objMatch.matchesBSON(BSON("a" << BSONObj())));
    ASSERT_TRUE(objMatch.matchesBSON(BSON("a" << BSON("b"
                                                      << "string"))));
    ASSERT_FALSE(objMatch.matchesBSON(BSON("a" << BSON_ARRAY(BSONObj()))));
}

TEST(InternalSchemaObjectMatchExpression, NestedObjectMatchReturnsCorrectPath) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "       b: {$_internalSchemaObjectMatch: {"
        "           $or: [{c: {$type: 'string'}}, {c: {$gt: 0}}]"
        "       }}}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->path(), "a");
    ASSERT_EQ(objMatch.getValue()->getChild(0)->path(), "b");
}

TEST(InternalSchemaObjectMatchExpression, MatchesNestedObjectMatch) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "       b: {$_internalSchemaObjectMatch: {"
        "           c: 3"
        "       }}}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_FALSE(objMatch.getValue()->matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(objMatch.getValue()->matchesBSON(fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(objMatch.getValue()->matchesBSON(fromjson("{a: {b: {c: 1}}}")));
    ASSERT_TRUE(objMatch.getValue()->matchesBSON(fromjson("{a: {b: {c: 3}}}")));
}

TEST(InternalSchemaObjectMatchExpression, EquivalentReturnsCorrectResults) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        b: 3"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher objectMatch(query, expCtx);

    query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        b: {$eq: 3}"
        "    }}}");
    Matcher objectMatchEq(query, expCtx);
    ASSERT_TRUE(objectMatch.getMatchExpression()->equivalent(objectMatchEq.getMatchExpression()));

    query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        c: {$eq: 3}"
        "    }}}");
    Matcher objectMatchNotEq(query, expCtx);
    ASSERT_FALSE(
        objectMatch.getMatchExpression()->equivalent(objectMatchNotEq.getMatchExpression()));
}

TEST(InternalSchemaObjectMatchExpression, SubExpressionRespectsCollator) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    auto query = fromjson(
        "{a: {$_internalSchemaObjectMatch: {"
        "	b: {$eq: 'FOO'}"
        "}}}");
    auto objectMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objectMatch.getStatus());

    ASSERT_TRUE(objectMatch.getValue()->matchesBSON(fromjson("{a: {b: 'FOO'}}")));
    ASSERT_TRUE(objectMatch.getValue()->matchesBSON(fromjson("{a: {b: 'foO'}}")));
    ASSERT_TRUE(objectMatch.getValue()->matchesBSON(fromjson("{a: {b: 'foo'}}")));
}

TEST(InternalSchemaObjectMatchExpression, RejectsArraysContainingMatchingSubObject) {
    auto query = fromjson("{a: {$_internalSchemaObjectMatch: {b: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_FALSE(objMatch.getValue()->matchesBSON(fromjson("{a: 1}")));
    ASSERT_TRUE(objMatch.getValue()->matchesBSON(fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(objMatch.getValue()->matchesBSON(fromjson("{a: [{b: 1}]}")));
    ASSERT_FALSE(objMatch.getValue()->matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
}

TEST(InternalSchemaObjectMatchExpression, HasSingleChild) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        c: {$eq: 3}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 1U);
    ASSERT(objMatch.getValue()->getChild(0));
}

DEATH_TEST_REGEX(InternalSchemaObjectMatchExpression,
                 GetChildFailsIndexGreaterThanOne,
                 "Tripwire assertion.*6400217") {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        c: {$eq: 3}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 1);
    ASSERT_THROWS_CODE(objMatch.getValue()->getChild(1), AssertionException, 6400217);
}

}  // namespace
}  // namespace mongo
