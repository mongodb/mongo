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

#include "mongo/db/exec/projection_exec_agg.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using ArrayRecursionPolicy = ProjectionExecAgg::ArrayRecursionPolicy;
using DefaultIdPolicy = ProjectionExecAgg::DefaultIdPolicy;

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

// Helper to simplify the creation of a ProjectionExecAgg which includes _id and recurses arrays.
std::unique_ptr<ProjectionExecAgg> makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
    BSONObj projSpec) {
    return ProjectionExecAgg::create(
        projSpec, DefaultIdPolicy::kIncludeId, ArrayRecursionPolicy::kRecurseNestedArrays);
}

// Helper to simplify the creation of a ProjectionExecAgg which excludes _id and recurses arrays.
std::unique_ptr<ProjectionExecAgg> makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
    BSONObj projSpec) {
    return ProjectionExecAgg::create(
        projSpec, DefaultIdPolicy::kExcludeId, ArrayRecursionPolicy::kRecurseNestedArrays);
}

std::set<FieldRef> toFieldRefs(const std::set<std::string>& stringPaths) {
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

TEST(ProjectionExecAggErrors, ShouldRejectMixOfInclusionAndComputedFields) {
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

TEST(ProjectionExecAggErrors, ShouldRejectMixOfExclusionAndComputedFields) {
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

TEST(ProjectionExecAggErrors, ShouldRejectOnlyComputedFields) {
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

TEST(ProjectionExecAggType, ShouldAcceptInclusionProjection) {
    auto parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("a" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << false << "a" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << false << "a.b.c" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("_id.x" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << BSON("x" << true)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("x" << BSON("_id" << true)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);
}

TEST(ProjectionExecAggType, ShouldAcceptExclusionProjection) {
    auto parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("a" << false));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("_id.x" << false));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("_id" << BSON("x" << false)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        BSON("x" << BSON("_id" << false)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(BSON("_id" << false));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);
}

// Misc tests.

TEST(ProjectionExecAggTests, InclusionFieldPathsWithImplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"_id", "a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecAggTests, InclusionFieldPathsWithExplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{_id: 1, a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"_id", "a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecAggTests, InclusionFieldPathsWithExplicitIdInclusionIdOnly) {
    auto parsedProject =
        makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(fromjson("{_id: 1}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"_id"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecAggTests, InclusionFieldPathsWithImplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecAggTests, InclusionFieldPathsWithExplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{_id: 0, a: {b: {c: 1}}, d: 1}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();
    std::set<FieldRef> expectedPaths = toFieldRefs({"a.b.c", "d"});

    // Verify that the exhaustive set of paths is as expected.
    ASSERT(exhaustivePaths == expectedPaths);
}

TEST(ProjectionExecAggTests, ExclusionFieldPathsWithImplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();

    // Verify that the exhaustive set is empty, despite the implicit inclusion of _id.
    ASSERT(exhaustivePaths.empty());
}

TEST(ProjectionExecAggTests, ExclusionFieldPathsWithExplicitIdInclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{_id: 1, a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();

    // Verify that the exhaustive set is empty, despite the explicit inclusion of _id.
    ASSERT(exhaustivePaths.empty());
}

TEST(ProjectionExecAggTests, ExclusionFieldPathsWithImplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdExclusionAndNestedArrayRecursion(
        fromjson("{a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();

    // Verify that the exhaustive set is empty.
    ASSERT(exhaustivePaths.empty());
}

TEST(ProjectionExecAggTests, ExclusionFieldPathsWithExplicitIdExclusion) {
    auto parsedProject = makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(
        fromjson("{_id: 1, a: {b: {c: 0}}, d: 0}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();

    // Verify that the exhaustive set is empty.
    ASSERT(exhaustivePaths.empty());
}

TEST(ProjectionExecAggTests, ExclusionFieldPathsWithExplicitIdExclusionIdOnly) {
    auto parsedProject =
        makeProjectionWithDefaultIdInclusionAndNestedArrayRecursion(fromjson("{_id: 0}"));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    // Extract the exhaustive set of paths that will be preserved by the projection.
    auto exhaustivePaths = parsedProject->getExhaustivePaths();

    // Verify that the exhaustive set is empty.
    ASSERT(exhaustivePaths.empty());
}

}  // namespace
}  // namespace mongo
