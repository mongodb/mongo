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

#include "mongo/db/exec/projection_executor_builder.h"

#include "mongo/base/exact_cast.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::projection_executor {
namespace {
/**
 * This test fixture run the test twice, one when the fast-path projection mode is allowed, another
 * one when it's not.
 *
 * The 'AllowFallBackToDefault' parameter should be set to 'true', if the executor is allowed to
 * fall back to the default inclusion projection implementation if the fast-path projection cannot
 * be used for a specific test. If set to 'false', a tassert will be triggered if fast-path
 * projection was expected to be chosen, but the default one has been picked instead.
 */
template <bool AllowFallBackToDefault>
class BaseProjectionExecutorTest : public AggregationContextFixture {
public:
    void run() {
        auto base = static_cast<mongo::unittest::Test*>(this);
        try {
            _allowFastPath = true;
            base->run();
            _allowFastPath = false;
            base->run();
        } catch (...) {
            LOGV2(20597,
                  "Exception while testing",
                  "allowFastPath"_attr = _allowFastPath,
                  "allowFallBackToDefault"_attr = AllowFallBackToDefault);
            throw;
        }
    }

protected:
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

        return projection_ast::parseAndAnalyze(getExpCtx(),
                                               projectionBson,
                                               swMatchExpression.getValue().get(),
                                               matchExprBson.get_value_or(BSONObj()),
                                               policies);
    }

    auto createProjectionExecutor(const projection_ast::Projection& projection) {
        const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

        auto builderParams{kDefaultBuilderParams};
        if (!_allowFastPath) {
            builderParams.reset(kAllowFastPath);
        }

        auto executor = buildProjectionExecutor(getExpCtx(), &projection, {}, builderParams);
        if (executor->getType() == TransformerInterface::TransformerType::kInclusionProjection) {
            assertFastPathHandledCorrectly<projection_executor::InclusionProjectionExecutor,
                                           projection_executor::FastPathEligibleInclusionNode>(
                executor.get());
        } else {
            assertFastPathHandledCorrectly<projection_executor::ExclusionProjectionExecutor,
                                           projection_executor::FastPathEligibleExclusionNode>(
                executor.get());
        }
        return executor;
    }

    template <typename ExecutorImpl, typename FastPathNode>
    void assertFastPathHandledCorrectly(projection_executor::ProjectionExecutor* executor) {
        auto executorImpl = static_cast<ExecutorImpl*>(executor);
        auto fastPathRootNode = exact_pointer_cast<FastPathNode*>(executorImpl->getRoot());
        if (_allowFastPath) {
            ASSERT_TRUE(fastPathRootNode || AllowFallBackToDefault);
        } else {
            ASSERT_FALSE(fastPathRootNode);
        }
    }

    // True, if the projection executor is allowed to use the fast-path inclusion projection
    // implementation.
    bool _allowFastPath{true};
};

using ProjectionExecutorTestWithFallBackToDefault = BaseProjectionExecutorTest<true>;
using ProjectionExecutorTestWithoutFallBackToDefault = BaseProjectionExecutorTest<false>;

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectInclusionWithIdPath) {
    auto projWithId = parseWithDefaultPolicies(fromjson("{a: 1, _id: 1}"));
    auto executor = createProjectionExecutor(projWithId);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{_id: 123, a: 'abc'}")},
                       executor->applyTransformation(
                           Document{fromjson("{_id: 123, a: 'abc', b: 'def', c: 'ghi'}")}));

    auto projWithoutId = parseWithDefaultPolicies(fromjson("{a: 1, _id: 0}"));
    executor = createProjectionExecutor(projWithoutId);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: 'abc'}")},
                       executor->applyTransformation(
                           Document{fromjson("{_id: 123, a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectInclusionUndottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: 1, b: 1}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: 'abc', b: 'def'}")},
        executor->applyTransformation(Document{fromjson("{a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectInclusionDottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{'a.b': 1, 'a.d': 1}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: 'abc', d: 'ghi'}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: 'abc', c: 'def', d: 'ghi'}}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectInclusionDottedPathNestedArrays) {
    auto proj = parseWithDefaultPolicies(fromjson("{'a.b': 1}"));
    auto executor = createProjectionExecutor(proj);
    Document input{fromjson("{a: [{b: 'abc', c: 'def'}, [{b: 'abc', c: 'def'}, 'd'], 'd']}")};
    BSONObj expected = fromjson("{a: [{b: 'abc'}, [{b: 'abc'}]]}");
    BSONObj found = executor->applyTransformation(input).toBsonWithMetaData();
    // Using BSONObj instead of Document because non-fastpath projection leaves missing values when
    // projecting scalar elements of array. Because of missing values in the array,
    // ASSERT_DOCUMENT_EQ consideres expected and found Documents different.
    ASSERT_BSONOBJ_EQ(expected, found);
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectExpression) {
    auto proj = parseWithDefaultPolicies(fromjson("{c: {$add: ['$a', '$b']}}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{c: 3}")},
                       executor->applyTransformation(Document{fromjson("{a: 1, b: 2}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectExpressionWithCommonParent) {
    auto proj = parseWithDefaultPolicies(
        fromjson("{'a.b.c': 1, 'b.c.d': 1, 'a.p.c' : {$add: ['$a.b.e', '$a.p']}, 'a.b.e': 1}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: {e: 4}, p: {c: 6}}}")},
                       executor->applyTransformation(Document{fromjson("{a: {b: {e: 4}, p: 2}}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectExclusionWithIdPath) {
    auto projWithoutId = parseWithDefaultPolicies(fromjson("{a: 0, _id: 0}"));
    auto executor = createProjectionExecutor(projWithoutId);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{b: 'def', c: 'ghi'}")},
                       executor->applyTransformation(
                           Document{fromjson("{_id: 123, a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectExclusionUndottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: 0, b: 0}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{c: 'ghi'}")},
        executor->applyTransformation(Document{fromjson("{a: 'abc', b: 'def', c: 'ghi'}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectExclusionDottedPath) {
    auto proj = parseWithDefaultPolicies(fromjson("{'a.b': 0, 'a.d': 0}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {c: 'def'}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: 'abc', c: 'def', d: 'ghi'}}")}));
}

TEST_F(ProjectionExecutorTestWithoutFallBackToDefault, CanProjectExclusionDottedPathNestedArrays) {
    auto proj = parseWithDefaultPolicies(fromjson("{'a.c': 0}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 'abc'}, [{b: 'abc'}, 'd'], 'd']}")},
                       executor->applyTransformation(Document{fromjson(
                           "{a: [{b: 'abc', c: 'def'}, [{b: 'abc', c: 'def'}, 'd'], 'd']}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindPositional) {
    auto proj =
        parseWithFindFeaturesEnabled(fromjson("{'a.b.$': 1}"), fromjson("{'a.b': {$gte: 3}}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: [3]}}")},
                       executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}}")}));

    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: [4]}}")},
                       executor->applyTransformation(Document{fromjson("{a: {b: [4, 3, 2]}}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindElemMatchWithInclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {b: {$gte: 3}}}, c: 1}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: [{b: 3}]}")},
        executor->applyTransformation(Document{fromjson("{a: [{b: 1}, {b: 2}, {b: 3}]}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindElemMatch) {
    const BSONObj obj = fromjson("{a: [{b: 3, c: 1}, {b: 1, c: 2}, {b: 1, c: 3}]}");
    {
        auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {b: 1}}}"));
        auto executor = createProjectionExecutor(proj);
        ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 1, c: 2}]}")},
                           executor->applyTransformation(Document{obj}));
    }

    {
        auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {b: 1, c: 3}}}"));
        auto executor = createProjectionExecutor(proj);
        ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 1, c: 3}]}")},
                           executor->applyTransformation(Document{obj}));
    }
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, ElemMatchRespectsCollator) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    getExpCtx()->setCollator(std::move(collator));

    auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {$gte: 'abc'}}}"));
    auto executor = createProjectionExecutor(proj);

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{ a: [ \"zdd\" ] }")},
        executor->applyTransformation(Document{fromjson("{a: ['zaa', 'zbb', 'zdd', 'zee']}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindElemMatchWithExclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {b: {$gte: 3}}}, c: 0}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 3}], d: 'def'}")},
                       executor->applyTransformation(Document{
                           fromjson("{a: [{b: 1}, {b: 2}, {b: 3}], c: 'abc', d: 'def'}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindSliceWithInclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, c: 1}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}, c: 'abc'}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3]}, c: 'abc'}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindSliceSkipLimitWithInclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, c: 1}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}, c: 'abc'}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: 'abc'}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindSliceBasicWithExclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: 3}, c: 0}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [1,2,3]}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: 'abc'}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindSliceSkipLimitWithExclusion) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, c: 0}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: 'abc'}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, CanProjectFindSliceAndPositional) {
    auto proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': {$slice: [1,2]}, 'c.$': 1}"),
                                             fromjson("{c: {$gte: 6}}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: {b: [2,3]}, c: [6]}")},
        executor->applyTransformation(Document{fromjson("{a: {b: [1,2,3,4]}, c: [5,6,7]}")}));
}

TEST_F(ProjectionExecutorTestWithFallBackToDefault, ExecutorOptimizesExpression) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: 1, b: {$add: [1, 2]}}"));
    auto executor = createProjectionExecutor(proj);
    ASSERT_DOCUMENT_EQ(Document{fromjson("{_id: true, a: true, b: {$const: 3}}")},
                       executor->serializeTransformation());
}
}  // namespace
}  // namespace mongo::projection_executor
