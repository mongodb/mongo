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
