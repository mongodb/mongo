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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo::projection_executor {
class ProjectionExecutorTest : public AggregationContextFixture {
public:
    projection_ast::Projection parseWithDefaultPolicies(
        const BSONObj& projectionBson, boost::optional<BSONObj> matchExprBson = boost::none) {
        return parseWithPolicies(projectionBson, matchExprBson, ProjectionPolicies{});
    }

    projection_ast::Projection parseWithFindFeaturesEnabled(
        const BSONObj& projectionBson, boost::optional<BSONObj> matchExprBson = boost::none) {
        auto policy = ProjectionPolicies::findProjectionPolicies();
        return parseWithPolicies(projectionBson, matchExprBson, policy);
    }

    projection_ast::Projection parseWithPolicies(const BSONObj& projectionBson,
                                                 boost::optional<BSONObj> matchExprBson,
                                                 ProjectionPolicies policies) {
        StatusWith<std::unique_ptr<MatchExpression>> swMatchExpression(nullptr);
        if (matchExprBson) {
            swMatchExpression = MatchExpressionParser::parse(*matchExprBson, getExpCtx());
            uassertStatusOK(swMatchExpression.getStatus());
        }

        return projection_ast::parse(getExpCtx(),
                                     projectionBson,
                                     swMatchExpression.getValue().get(),
                                     matchExprBson.get_value_or(BSONObj()),
                                     policies);
    }
};

TEST_F(ProjectionExecutorTest, CanProjectInclusionWithIdPath) {
    auto projWithId = parseWithDefaultPolicies(fromjson("{a: 1, _id: 1}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &projWithId, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{_id: 123, a: 'abc'}")},
                       executor->applyTransformation(
                           Document{fromjson("{_id: 123, a: 'abc', b: 'def', c: 'ghi'}")}));

    auto projWithoutId = parseWithDefaultPolicies(fromjson("{a: 1, _id: 0}"));
    executor = buildProjectionExecutor(getExpCtx(), &projWithoutId, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: 'abc'}")},
                       executor->applyTransformation(
                           Document{fromjson("{_id: 123, a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectInclusionUndottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: 1, b: 1}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: 'abc', b: 'def'}")},
        executor->applyTransformation(Document{fromjson("{a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectInclusionDottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{'a.b': 1, 'a.d': 1}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: 'abc', d: 'ghi'}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: 'abc', c: 'def', d: 'ghi'}}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectExpression) {
    auto proj = parseWithDefaultPolicies(fromjson("{c: {$add: ['$a', '$b']}}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{c: 3}")},
                       executor->applyTransformation(Document{fromjson("{a: 1, b: 2}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectExclusionWithIdPath) {
    auto projWithoutId = parseWithDefaultPolicies(fromjson("{a: 0, _id: 0}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &projWithoutId, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{b: 'def', c: 'ghi'}")},
                       executor->applyTransformation(
                           Document{fromjson("{_id: 123, a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectExclusionUndottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: 0, b: 0}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{c: 'ghi'}")},
        executor->applyTransformation(Document{fromjson("{a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectExclusionDottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{'a.b': 0, 'a.d': 0}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {c: 'def'}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: 'abc', c: 'def', d: 'ghi'}}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectFindPositional) {
    auto proj =
        parseWithFindFeaturesEnabled(fromjson("{'a.b.$': 1}"), fromjson("{'a.b': {$gte: 3}}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: [3]}}")},
                       executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectFindElemMatchWithInclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {b: {$gte: 3}}}, c: 1}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: [{b: 3}]}")},
        executor->applyTransformation(Document{fromjson("{a: [{b: 1}, {b: 2}, {b: 3}]}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectFindElemMatchWithExclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {b: {$gte: 3}}}, c: 0}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 3}], d: 'def'}")},
                       executor->applyTransformation(Document{
                           fromjson("{a: [{b: 1}, {b: 2}, {b: 3}], c: 'abc', d: 'def'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectFindSliceWithInclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, c: 1}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}, c: 'abc'}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: 'abc'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectFindSliceWithExclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, c: 0}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: 'abc'}")}));
}

TEST_F(ProjectionExecutorTest, CanProjectFindSliceAndPositional) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, 'c.$': 1}"),
                                             fromjson("{c: {$gte: 6}}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}, c: [6]}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: [5,6,7]}")}));
}

TEST_F(ProjectionExecutorTest, ExecutorOptimizesExpression) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: 1, b: {$add: [1, 2]}}"));
    auto executor = buildProjectionExecutor(getExpCtx(), &proj, {});
    ASSERT_DOCUMENT_EQ(Document{fromjson("{_id: true, a: true, b: {$const: 3}}")},
                       executor->serializeTransformation(boost::none));
}
}  // namespace mongo::projection_executor
