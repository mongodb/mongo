/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <map>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace {

using namespace mongo;

using mongo::projection_ast::Projection;
using mongo::projection_ast::ProjectType;

class ProjectionASTTest : public AggregationContextFixture {
public:
    Projection parseWithDefaultPolicies(const BSONObj& projectionBson,
                                        boost::optional<BSONObj> matchExprBson = boost::none) {
        StatusWith<std::unique_ptr<MatchExpression>> swMatchExpression(nullptr);
        if (matchExprBson) {
            swMatchExpression = MatchExpressionParser::parse(*matchExprBson, getExpCtx());
            ASSERT_OK(swMatchExpression.getStatus());
        }

        return projection_ast::parse(getExpCtx(),
                                     projectionBson,
                                     swMatchExpression.getValue().get(),
                                     matchExprBson.get_value_or(BSONObj()),
                                     ProjectionPolicies{});
    }
};

void assertCanClone(Projection proj) {
    boost::optional<Projection> optProj(std::move(proj));

    auto clone = optProj->root()->clone();

    auto originalBSON = projection_ast::astToDebugBSON(optProj->root());
    auto cloneBSON = projection_ast::astToDebugBSON(clone.get());
    ASSERT_BSONOBJ_EQ(originalBSON, cloneBSON);

    // Delete the original.
    optProj = boost::none;

    // Make sure we can still serialize the clone.
    auto cloneBSON2 = projection_ast::astToDebugBSON(clone.get());
    ASSERT_BSONOBJ_EQ(cloneBSON, cloneBSON2);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: 1, b: 1}"));
    ASSERT(proj.type() == ProjectType::kInclusion);

    Projection projWithoutId = parseWithDefaultPolicies(fromjson("{_id: 0, b: 1}"));
    ASSERT(projWithoutId.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusionWithNesting) {
    Projection proj = parseWithDefaultPolicies(fromjson("{'a.b': 1, 'a.c': 1}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusion) {
    Projection p = parseWithDefaultPolicies(fromjson("{a: 0, _id: 0}"));
    ASSERT(p.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestInvalidProjectionWithInclusionsAndExclusions) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: 1, b: 0}")), DBException, 31254);
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: 0, b: 1}")), DBException, 31253);
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: 0, b: {$add: [1, 2]}}")), DBException, 31252);
}

TEST_F(ProjectionASTTest, TestInvalidProjectionWithPositionalAndElemMatch) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{'a.$': 1, b: {$elemMatch: {foo: 'bar'}}}"),
                                 fromjson("{a: 1}")),
        DBException,
        31255);

    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{b: {$elemMatch: {foo: 'bar'}}, 'a.$': 1}"),
                                 fromjson("{a: 1}")),
        DBException,
        31256);
}

TEST_F(ProjectionASTTest, TestCloningWithPositionalAndSlice) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, 'c.d.$': 1, f: {$slice: [1, 2]}}"),
                                 fromjson("{'c.d': {$gt: 1}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
    assertCanClone(std::move(proj));
}

// Can't use $elemMatch in projections which use the positional projection.
TEST_F(ProjectionASTTest, TestCloningWithElemMatch) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$elemMatch: {foo: 'bar'}}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
    assertCanClone(std::move(proj));
}

TEST_F(ProjectionASTTest, TestCloningWithExpression) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$add: [1, 2, '$a']}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
    assertCanClone(std::move(proj));
}

TEST_F(ProjectionASTTest, TestDebugBSONWithPositionalAndSliceSkipLimit) {
    Projection proj = parseWithDefaultPolicies(
        fromjson("{'a.b': 1, b: 1, 'c.d.$': 1, f: {$slice: [1, 2]}}"), fromjson("{'c.d': 1}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson(
        "{a: {b: true}, b: true, c: {'d.$': {'c.d': {$eq: 1}}}, f: {$slice: [1, 2]}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithSliceLimit) {
    Projection proj = parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$slice: 2}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: true}, b: true, f: {$slice: 2}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithElemMatch) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$elemMatch: {foo: 'bar'}}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected =
        fromjson("{a: {b: true}, b: true, f: {$elemMatch: {foo: {$eq: 'bar'}}}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithExpression) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$add: [1, 2, '$a']}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected =
        fromjson("{a: {b: true}, b: true, f: {$add: [{$const: 1}, {$const: 2}, '$a']}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, ParserErrorsOnCollisionNestedFieldFirst) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{'a.b': 1, d: 1, a: 1}")), DBException, 31250);
}

TEST_F(ProjectionASTTest, ParserErrorsOnCollisionNestedFieldLast) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: 1, d: 1, 'a.b': 1}")), DBException, 31249);
}

TEST_F(ProjectionASTTest, ParserErrorsOnInvalidSlice) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: {$slice: ['not a number', 123]}}")),
                       DBException,
                       31257);

    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: {$slice: [123, 'not a number']}}")),
                       DBException,
                       31258);

    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: {$slice: [123, -5]}}")), DBException, 31259);
}

}  // namespace
