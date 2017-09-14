/**
 *    Copyright (C) 2017 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchesEmptyQuery) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchesValidQueries) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4))));

    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4))));
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(10 << 2 << 3 << 4))));
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(10 << 20 << 3 << 4))));
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3 << 40))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, RejectsNonArrayElements) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_FALSE(expr.getValue()->matchesBSON(BSON("a" << BSON("a" << 1))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchesArraysWithLessElementsThanIndex) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, NestedArraysMatchSubexpression) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(
        expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << BSON_ARRAY(3 << 4) << 4))));
    ASSERT_TRUE(
        expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << BSON_ARRAY(6 << 4) << 4))));
    ASSERT_FALSE(
        expr.getValue()->matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << BSON_ARRAY(5 << 6) << 4))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, MatchedQueriesWithDottedPaths) {
    auto query = fromjson("{'a.b': {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    ASSERT_TRUE(
        expr.getValue()->matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3 << 4)))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, HasSingleChild) {
    auto query = fromjson("{'a.b': {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 1U);
    ASSERT(objMatch.getValue()->getChild(0));
}

DEATH_TEST(InternalSchemaAllElemMatchFromIndexMatchExpression,
           GetChildFailsIndexGreaterThanOne,
           "Invariant failure i == 0") {
    auto query = fromjson("{'a.b': {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    objMatch.getValue()->getChild(1);
}

}  // namespace
}  // namespace mongo
