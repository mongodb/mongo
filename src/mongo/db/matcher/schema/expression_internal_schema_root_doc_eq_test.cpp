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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(InternalSchemaRootDocEqMatchExpression, MatchesObject) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(BSON("a" << 1 << "b"
                            << "string"));
    ASSERT_TRUE(rootDocEq.matchesBSON(BSON("a" << 1 << "b"
                                               << "string")));
    ASSERT_FALSE(rootDocEq.matchesBSON(BSON("a" << 2 << "b"
                                                << "string")));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesNestedObject) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(BSON("a" << 1 << "b" << BSON("c" << 1)));
    ASSERT_TRUE(rootDocEq.matchesBSON(BSON("a" << 1 << "b" << BSON("c" << 1))));
    ASSERT_FALSE(rootDocEq.matchesBSON(BSON("a" << 1 << "b" << BSON("c" << 2))));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesObjectIgnoresElementOrder) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(BSON("a" << 1 << "b" << BSON("c" << 1)));
    ASSERT_TRUE(rootDocEq.matchesBSON(BSON("b" << BSON("c" << 1) << "a" << 1)));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesNestedObjectIgnoresElementOrder) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(BSON("a" << BSON("b" << 1 << "c" << 1)));
    ASSERT_TRUE(rootDocEq.matchesBSON(BSON("a" << BSON("c" << 1 << "b" << 1))));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesEmptyObject) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(BSONObj());
    ASSERT_TRUE(rootDocEq.matchesBSON(BSONObj()));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesNestedArray) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(BSON("a" << BSON_ARRAY(1 << 2 << 3)));
    ASSERT_TRUE(rootDocEq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3))));
    ASSERT_FALSE(rootDocEq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 3 << 2))));
}

TEST(InternalSchemaRootDocEqMatchExpression, MatchesObjectWithNullElement) {
    InternalSchemaRootDocEqMatchExpression rootDocEq;
    rootDocEq.init(fromjson("{a: null}"));
    ASSERT_TRUE(rootDocEq.matchesBSON(fromjson("{a: null}")));
    ASSERT_FALSE(rootDocEq.matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(rootDocEq.matchesBSON(fromjson("{}")));
    ASSERT_FALSE(rootDocEq.matchesBSON(fromjson("{a: undefined}")));
}

TEST(InternalSchemaRootDocEqMatchExpression, EquivalentReturnsCorrectResults) {
    auto query = fromjson(R"(
             {$_internalSchemaRootDocEq: {
                 b: 1, c: 1
             }})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher rootDocEq(query, expCtx);

    query = fromjson(R"(
             {$_internalSchemaRootDocEq: {
                 c: 1, b: 1
             }})");
    Matcher exprEq(query, expCtx);
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(exprEq.getMatchExpression()));

    query = fromjson(R"(
             {$_internalSchemaRootDocEq: {
                 c: 1
             }})");
    Matcher exprNotEq(query, expCtx);
    ASSERT_FALSE(rootDocEq.getMatchExpression()->equivalent(exprNotEq.getMatchExpression()));
}

TEST(InternalSchemaRootDocEqMatchExpression, EquivalentToClone) {
    auto query = fromjson("{$_internalSchemaRootDocEq: {a:1, b: {c: 1, d: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher rootDocEq(query, expCtx);
    auto clone = rootDocEq.getMatchExpression()->shallowClone();
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(clone.get()));
}
}  // namespace
}  // namespace mongo
