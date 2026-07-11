// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

TEST(InternalSchemaObjectMatchExpression, NestedObjectMatchReturnsCorrectPath) {
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "       b: {$_internalSchemaObjectMatch: {"
        "           $or: [{c: {$type: 'string'}}, {c: {$gt: 0}}]"
        "       }}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->path(), "a");
    ASSERT_EQ(objMatch.getValue()->getChild(0)->path(), "b");
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

DEATH_TEST_REGEX(InternalSchemaObjectMatchExpressionDeathTest,
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
