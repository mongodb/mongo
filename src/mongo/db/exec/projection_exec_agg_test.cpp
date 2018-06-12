/**
 * Copyright (C) 2018 MongoDB, Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/projection_exec_agg.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

//
// Error cases.
//

TEST(ProjectionExecAggErrors, ShouldRejectMixOfInclusionAndComputedFields) {
    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a" << true << "b" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a" << wrapInLiteral(1) << "b" << true)),
                  AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a.b" << true << "a.c" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a.b" << wrapInLiteral(1) << "a.c" << true)),
                  AssertionException);

    ASSERT_THROWS(
        ProjectionExecAgg::create(BSON("a" << BSON("b" << true << "c" << wrapInLiteral(1)))),
        AssertionException);

    ASSERT_THROWS(
        ProjectionExecAgg::create(BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << true))),
        AssertionException);
}

TEST(ProjectionExecAggErrors, ShouldRejectMixOfExclusionAndComputedFields) {
    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a" << false << "b" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a" << wrapInLiteral(1) << "b" << false)),
                  AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a.b" << false << "a.c" << wrapInLiteral(1))),
                  AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(BSON("a.b" << wrapInLiteral(1) << "a.c" << false)),
                  AssertionException);

    ASSERT_THROWS(
        ProjectionExecAgg::create(BSON("a" << BSON("b" << false << "c" << wrapInLiteral(1)))),
        AssertionException);

    ASSERT_THROWS(
        ProjectionExecAgg::create(BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << false))),
        AssertionException);
}

TEST(ProjectionExecAggErrors, ShouldRejectOnlyComputedFields) {
    ASSERT_THROWS(
        ProjectionExecAgg::create(BSON("a" << wrapInLiteral(1) << "b" << wrapInLiteral(1))),
        AssertionException);

    ASSERT_THROWS(
        ProjectionExecAgg::create(BSON("a.b" << wrapInLiteral(1) << "a.c" << wrapInLiteral(1))),
        AssertionException);

    ASSERT_THROWS(ProjectionExecAgg::create(
                      BSON("a" << BSON("b" << wrapInLiteral(1) << "c" << wrapInLiteral(1)))),
                  AssertionException);
}

// Valid projections.

TEST(ProjectionExecAggType, ShouldAcceptInclusionProjection) {
    auto parsedProject = ProjectionExecAgg::create(BSON("a" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id" << false << "a" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id" << false << "a.b.c" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id.x" << true));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id" << BSON("x" << true)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("x" << BSON("_id" << true)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kInclusionProjection);
}

TEST(ProjectionExecAggType, ShouldAcceptExclusionProjection) {
    auto parsedProject = ProjectionExecAgg::create(BSON("a" << false));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id.x" << false));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id" << BSON("x" << false)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("x" << BSON("_id" << false)));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);

    parsedProject = ProjectionExecAgg::create(BSON("_id" << false));
    ASSERT(parsedProject->getType() == ProjectionExecAgg::ProjectionType::kExclusionProjection);
}

}  // namespace
}  // namespace mongo
