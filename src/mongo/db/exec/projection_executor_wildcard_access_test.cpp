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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/exec/projection_executor_utils.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::projection_executor {
namespace {
template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

auto createProjectionExecutor(const BSONObj& spec, const ProjectionPolicies& policies) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto projection = projection_ast::parseAndAnalyze(expCtx, spec, policies);
    auto executor = buildProjectionExecutor(expCtx, &projection, policies, kDefaultBuilderParams);
    return executor;
}

// Helper to simplify the creation of a ProjectionExecutor which includes _id and recurses arrays.
std::unique_ptr<ProjectionExecutor> makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
    BSONObj projSpec) {
    ProjectionPolicies policies{ProjectionPolicies::DefaultIdPolicy::kIncludeId,
                                ProjectionPolicies::ArrayRecursionPolicy::kRecurseNestedArrays,
                                ProjectionPolicies::ComputedFieldsPolicy::kBanComputedFields,
                                ProjectionPolicies::FindOnlyFeaturesPolicy::kBanFindOnlyFeatures};
    return createProjectionExecutor(projSpec, policies);
}

// Helper to simplify the creation of a ProjectionExecutor which excludes _id and recurses arrays.
std::unique_ptr<ProjectionExecutor> makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
    BSONObj projSpec) {
    ProjectionPolicies policies{ProjectionPolicies::DefaultIdPolicy::kExcludeId,
                                ProjectionPolicies::ArrayRecursionPolicy::kRecurseNestedArrays,
                                ProjectionPolicies::ComputedFieldsPolicy::kBanComputedFields,
                                ProjectionPolicies::FindOnlyFeaturesPolicy::kBanFindOnlyFeatures};
    return createProjectionExecutor(projSpec, policies);
}

std::set<FieldRef> toFieldRefs(const OrderedPathSet& stringPaths) {
    std::set<FieldRef> fieldRefs;
    std::transform(stringPaths.begin(),
                   stringPaths.end(),
                   std::inserter(fieldRefs, fieldRefs.begin()),
                   [](const auto& path) { return FieldRef(path); });
    return fieldRefs;
}

//
// Error cases.
//

TEST(ProjectionExecutorErrors, ShouldRejectMixOfInclusionAndComputedFields) {
    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << true << "b" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << wrapInLiteral(1) << "b" << true)),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a.b" << true << "a.c" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a.b" << wrapInLiteral(1) << "a.c" << true)),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << BSON("b" << true << "c" << wrapInLiteral(1)))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << true))),
                  AssertionException);
}

TEST(ProjectionExecutorErrors, ShouldRejectMixOfExclusionAndComputedFields) {
    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << false << "b" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << wrapInLiteral(1) << "b" << false)),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a.b" << false << "a.c" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a.b" << wrapInLiteral(1) << "a.c" << false)),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << BSON("b" << false << "c" << wrapInLiteral(1)))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << false))),
                  AssertionException);
}

TEST(ProjectionExecutorErrors, ShouldRejectOnlyComputedFields) {
    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << wrapInLiteral(1) << "b" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a.b" << wrapInLiteral(1) << "a.c" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << wrapInLiteral(1)))),
                  AssertionException);
}

// Valid projections.

TEST(ProjectionExecutorType, ShouldAcceptInclusionProjection) {
    auto parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("a" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << false << "a" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << false << "a.b.c" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("_id.x" << true));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << BSON("x" << true)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("x" << BSON("_id" << true)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);
}

TEST(ProjectionExecutorType, ShouldAcceptExclusionProjection) {
    auto parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("a" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("_id.x" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << BSON("x" << false)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("x" << BSON("_id" << false)));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("_id" << false));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);
}

// Misc tests.

TEST(ProjectionExecutorTests, InclusionFieldPathsWithImplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"_id", "a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(*exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecutorTests, InclusionFieldPathsWithExplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{_id: 1, a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"_id", "a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(*exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecutorTests, InclusionFieldPathsWithExplicitIdInclusionIdOnly) {
    auto parsedProject =
        makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(fromjson("{_id: 1}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"_id"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(*exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecutorTests, InclusionFieldPathsWithImplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(*exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecutorTests, InclusionFieldPathsWithExplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{_id: 0, a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(*exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecutorTests, ExclusionFieldPathsWithImplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();

    // Verify that the exhaustive set is not available, despite the implicit inclusion of _id.
    ASSERT(!exhaustivePaths);
}

TEST(ProjectionExecutorTests, ExclusionFieldPathsWithExplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{_id: 1, a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();

    // Verify that the exhaustive set is not available, despite the explicit inclusion of _id.
    ASSERT(!exhaustivePaths);
}

TEST(ProjectionExecutorTests, ExclusionFieldPathsWithImplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();

    // Verify that the exhaustive set is empty.
    ASSERT(!exhaustivePaths);
}

TEST(ProjectionExecutorTests, ExclusionFieldPathsWithExplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{_id: 1, a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();

    // Verify that the exhaustive set is empty.
    ASSERT(!exhaustivePaths);
}

TEST(ProjectionExecutorTests, ExclusionFieldPathsWithExplicitIdExclusionIdOnly) {
    auto parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(fromjson("{_id: 0}"));
    ASSERT(parsedProject->getType() == TransformerInterface::TransformerType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->extractExhaustivePaths();

    // Verify that the exhaustive set is empty.
    ASSERT(!exhaustivePaths);
}

}  // namespace
}  // namespace mongo::projection_executor
