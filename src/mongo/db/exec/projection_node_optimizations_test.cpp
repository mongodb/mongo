/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

// Unit tests for ProjectionNode optimizations:
//   - Opt 1: Pre-computed hash reuse in applyProjections and _applyProjections
//   - Opt 2: Cached dispatch table (_orderedAdditions) in applyExpressions
//   - Opt 3: Single-lookup getExpressionForPath
//   - Opt 5: size_t _maxFieldsToProject early-exit sentinel
//   - Opt 6: Position-based Document access in child expression dispatch

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::projection_executor {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

auto makeExpCtx() {
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest()};
}

// Builds an InclusionProjectionExecutor through the standard builder path, which calls
// optimize() internally.
auto makeInclusionExecutor(const boost::intrusive_ptr<ExpressionContextForTest>& expCtx,
                           const BSONObj& spec) {
    auto projection = projection_ast::parseAndAnalyze(expCtx, spec, ProjectionPolicies{});
    return buildProjectionExecutor(
        expCtx, &projection, ProjectionPolicies{}, kDefaultBuilderParams);
}

// Builds an ExclusionProjectionExecutor through the standard builder path.
auto makeExclusionExecutor(const boost::intrusive_ptr<ExpressionContextForTest>& expCtx,
                           const BSONObj& spec) {
    ProjectionPolicies policies;
    auto projection = projection_ast::parseAndAnalyze(expCtx, spec, policies);
    return buildProjectionExecutor(expCtx, &projection, policies, kDefaultBuilderParams);
}

// ---------------------------------------------------------------------------
// Pre-computed hash reuse — correctness of inclusion/exclusion projections.
// ---------------------------------------------------------------------------

// Inclusion projection with several fields exercises the path where hashed_key is computed
// once and reused for both the _projectedFieldsSet and _children lookups.
TEST(ProjectionNodeHashReuse, InclusionProjectionRetainsCorrectFields) {
    auto expCtx = makeExpCtx();
    auto executor =
        makeInclusionExecutor(expCtx, BSON("_id" << 0 << "a" << 1 << "b" << 1 << "c" << 1));

    auto result = executor->applyTransformation(Document{{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}});
    ASSERT_DOCUMENT_EQ(result, (Document{{"a", 1}, {"b", 2}, {"c", 3}}));
}

// Fields absent from the projection spec should be excluded.
TEST(ProjectionNodeHashReuse, InclusionProjectionExcludesNonProjectedFields) {
    auto expCtx = makeExpCtx();
    auto executor = makeInclusionExecutor(expCtx, BSON("_id" << 0 << "x" << 1));

    auto result = executor->applyTransformation(
        Document{{"x", 10}, {"y", 20}, {"z", 30}, {"w", 40}, {"v", 50}});
    ASSERT_DOCUMENT_EQ(result, (Document{{"x", 10}}));
}

// Exclusion projection: fields in the spec are removed, others are kept.
TEST(ProjectionNodeHashReuse, ExclusionProjectionRemovesSpecifiedFields) {
    auto expCtx = makeExpCtx();
    auto executor = makeExclusionExecutor(expCtx, BSON("d" << 0 << "e" << 0));

    auto result =
        executor->applyTransformation(Document{{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}});
    ASSERT_DOCUMENT_EQ(result, (Document{{"a", 1}, {"b", 2}, {"c", 3}}));
}

// ---------------------------------------------------------------------------
// Single-lookup getExpressionForPath.
// ---------------------------------------------------------------------------

// getExpressionForPath should return the stored expression without requiring a second lookup.
TEST(ProjectionNodeGetExpression, ReturnsExpressionForTopLevelPath) {
    auto expCtx = makeExpCtx();
    auto executor = AddFieldsProjectionExecutor::create(expCtx, BSON("computed" << "$src"));
    const auto& root = executor->getRoot();

    auto expr = root.getExpressionForPath(FieldPath("computed"));
    ASSERT_TRUE(expr != nullptr);
    // Verify the expression evaluates correctly on a sample document.
    auto result =
        expr->evaluate(Document{{"src", Value{99}}}, &expr->getExpressionContext()->variables);
    ASSERT_VALUE_EQ(result, Value{99});
}

// Querying a path that does not exist should return nullptr without crashing.
TEST(ProjectionNodeGetExpression, ReturnsNullForMissingPath) {
    auto expCtx = makeExpCtx();
    auto executor = AddFieldsProjectionExecutor::create(expCtx, BSON("a" << "$x"));
    const auto& root = executor->getRoot();

    ASSERT_TRUE(root.getExpressionForPath(FieldPath("b")) == nullptr);
}

// For a nested path, the expression should be found at the correct depth.
TEST(ProjectionNodeGetExpression, ReturnsExpressionForNestedPath) {
    auto expCtx = makeExpCtx();
    auto executor = AddFieldsProjectionExecutor::create(expCtx, BSON("outer.inner" << "$val"));
    const auto& root = executor->getRoot();

    auto expr = root.getExpressionForPath(FieldPath("outer.inner"));
    ASSERT_TRUE(expr != nullptr);
}

// ---------------------------------------------------------------------------
// _maxFieldsToProject early-exit sentinel.
// ---------------------------------------------------------------------------

// An inclusion projection over a wide document should stop reading once all projected
// fields have been found.
TEST(ProjectionNodeMaxFieldsEarlyExit, InclusionProjectionOnWideDocument) {
    auto expCtx = makeExpCtx();
    auto executor = makeInclusionExecutor(expCtx, BSON("_id" << 0 << "a" << 1 << "b" << 1));

    // Document has many extra fields; the projection should only retain a and b.
    auto result = executor->applyTransformation(
        Document{{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}, {"g", 7}});
    ASSERT_DOCUMENT_EQ(result, (Document{{"a", 1}, {"b", 2}}));
}

// Early exit must not drop fields that appear before the limit is reached.
TEST(ProjectionNodeMaxFieldsEarlyExit, InclusionProjectionRetainsAllProjectedFields) {
    auto expCtx = makeExpCtx();
    auto executor = makeInclusionExecutor(
        expCtx, BSON("_id" << 0 << "a" << 1 << "b" << 1 << "c" << 1 << "d" << 1));

    auto result = executor->applyTransformation(
        Document{{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"extra1", 5}, {"extra2", 6}});
    ASSERT_DOCUMENT_EQ(result, (Document{{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}}));
}

// A projection where some projected fields are absent from the document should still be correct.
TEST(ProjectionNodeMaxFieldsEarlyExit, InclusionProjectionWithMissingFields) {
    auto expCtx = makeExpCtx();
    auto executor = makeInclusionExecutor(expCtx, BSON("_id" << 0 << "a" << 1 << "missing" << 1));

    auto result = executor->applyTransformation(Document{{"a", 1}, {"b", 2}, {"c", 3}});
    ASSERT_DOCUMENT_EQ(result, (Document{{"a", 1}}));
}

// ---------------------------------------------------------------------------
// Position-based Document access in applyExpressions child dispatch.
// ---------------------------------------------------------------------------

// When a child ProjectionNode has computed fields, the fast path should read and write the
// nested value via position-based access, producing the same result as the fallback path.
TEST(ProjectionNodePositionBasedAccess, NestedComputedFieldFastPath) {
    auto expCtx = makeExpCtx();
    const BSONObj spec = BSON("outer.inner" << "$src");
    const Document inputDoc{{"src", 100}, {"outer", Document{{"inner", 0}, {"other", 1}}}};

    // Fallback path (no optimize).
    auto execFallback = AddFieldsProjectionExecutor::create(expCtx, spec);
    auto resultFallback = execFallback->applyProjection(inputDoc);

    // Fast path (with optimize, builds _orderedAdditions with child entries).
    auto execFast = AddFieldsProjectionExecutor::create(expCtx, spec);
    execFast->optimize();
    auto resultFast = execFast->applyProjection(inputDoc);

    ASSERT_DOCUMENT_EQ(resultFallback, resultFast);
    // The inner field should be set to 100 (from $src).
    ASSERT_VALUE_EQ(resultFast["outer"]["inner"], Value{100});
}

// Nested field computation when the outer field does not yet exist in the document.
TEST(ProjectionNodePositionBasedAccess, NestedComputedFieldOnMissingParent) {
    auto expCtx = makeExpCtx();
    const BSONObj spec = BSON("newNested.value" << "$x");
    const Document inputDoc{{"x", 7}};

    auto execFallback = AddFieldsProjectionExecutor::create(expCtx, spec);
    auto resultFallback = execFallback->applyProjection(inputDoc);

    auto execFast = AddFieldsProjectionExecutor::create(expCtx, spec);
    execFast->optimize();
    auto resultFast = execFast->applyProjection(inputDoc);

    ASSERT_DOCUMENT_EQ(resultFallback, resultFast);
    ASSERT_VALUE_EQ(resultFast["newNested"]["value"], Value{7});
}

// Multiple nested computed fields on the same parent exercise the child path in the dispatch
// table with multiple siblings.
TEST(ProjectionNodePositionBasedAccess, MultipleNestedComputedFieldsSiblings) {
    auto expCtx = makeExpCtx();
    const BSONObj spec = BSON("obj.p" << "$a"
                                      << "obj.q"
                                      << "$b");
    const Document inputDoc{{"a", 10}, {"b", 20}};

    auto execFallback = AddFieldsProjectionExecutor::create(expCtx, spec);
    auto resultFallback = execFallback->applyProjection(inputDoc);

    auto execFast = AddFieldsProjectionExecutor::create(expCtx, spec);
    execFast->optimize();
    auto resultFast = execFast->applyProjection(inputDoc);

    ASSERT_DOCUMENT_EQ(resultFallback, resultFast);
    ASSERT_VALUE_EQ(resultFast["obj"]["p"], Value{10});
    ASSERT_VALUE_EQ(resultFast["obj"]["q"], Value{20});
}

}  // namespace
}  // namespace mongo::projection_executor
