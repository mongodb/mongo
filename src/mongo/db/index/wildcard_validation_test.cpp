/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/index/wildcard_validation.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(WildcardValidation, GoodCase) {
    ASSERT_OK(validateWildcardIndex(BSON("$**" << 1)));
    ASSERT_OK(validateWildcardIndex(BSON("a" << 1 << "b" << 1 << "c.$**" << 1)));
}

TEST(WildcardValidation, Overlapping) {
    ASSERT_NOT_OK(validateWildcardIndex(BSON("a" << 1 << "b.c" << 1 << "b.$**" << 1)));
}

// Test normal cases for wildcardProjections.
TEST(WildcardProjectionValidation, GoodCase) {
    ASSERT_OK(validateWildcardProjection(BSON("c.d.e" << 1 << "a.b" << 1 << "$**" << 1),
                                         BSON("e" << 1 << "c.d.e.l" << 1 << "a.b.c" << 1)));
    ASSERT_OK(validateWildcardProjection(BSON("c.d.e" << 1 << "a.b" << 1 << "$**" << 1),
                                         BSON("a" << 0 << "c.d.e" << 0)));
    ASSERT_OK(validateWildcardProjection(BSON("c.d.e" << 1 << "a.b" << 1 << "$**" << 1),
                                         BSON("_id" << 1 << "a" << 0 << "c.d.e" << 0)));
    ASSERT_OK(
        validateWildcardProjection(BSON("$**" << 1), BSON("_id" << 0 << "a.b" << 1 << "e" << 1)));
}

// Wildcard prjection cannot be empty if specified for a Compound Wildcard Index.
TEST(WildcardProjectionValidation, EmptyWildcardProjection) {
    ASSERT_NOT_OK(
        validateWildcardProjection(BSON("c.d.e" << 1 << "a.b" << 1 << "$**" << 1), BSONObj()));
}

// Validation should fail if wildcardProjection paths are overlapping with regular index fields.
TEST(WildcardProjectionValidation, Overlapping_RegularIndexFields_WildcarProjections) {
    ASSERT_NOT_OK(validateWildcardProjection(BSON("a" << 1 << "c" << 1 << "$**" << 1),
                                             BSON("b" << 1 << "c" << 1)));
    ASSERT_NOT_OK(validateWildcardProjection(BSON("c.d.e.L" << 1 << "a.d.c" << 1 << "$**" << 1),
                                             BSON("e" << 1 << "c.d.e" << 1 << "a.b" << 1)));
}

// Validation should fail if wildcardProjection paths are overlapping with regular index fields.
TEST(WildcardProjectionValidation, OverlappingFields_IndexWildcardProjectionNotExcluding) {
    ASSERT_NOT_OK(validateWildcardProjection(BSON("d" << 1 << "c" << 1 << "$**" << 1),
                                             BSON("b" << 0 << "c" << 0)));
    ASSERT_NOT_OK(validateWildcardProjection(BSON("c.d.e" << 1 << "a.d.c" << 1 << "$**" << 1),
                                             BSON("e" << 0 << "c.d.e.l" << 0 << "a.d" << 0)));
}

TEST(WildcardProjectionValidation, OverlappingFields_NoOverlapping_WithExcludedFields) {
    ASSERT_OK(validateWildcardProjection(BSON("c.d.e.l" << 1 << "a.b.c" << 1 << "$**" << 1),
                                         BSON("e" << 0 << "c.d.e" << 0 << "a.b" << 0)));
}

TEST(WildcardProjectionValidation, OverlappingFields_WildcardProjectionOvelapping_Include) {
    ASSERT_NOT_OK(validateWildcardProjection(
        BSON("$**" << 1), BSON("a.b" << 1 << "d" << 1 << "e" << 1 << "a.b.c" << 1)));
}

TEST(WildcardProjectionValidation, OverlappingFields_WildcardProjectionOvelapping_Exclude) {
    ASSERT_NOT_OK(validateWildcardProjection(
        BSON("$**" << 1), BSON("a.b" << 0 << "e" << 0 << "d" << 0 << "a.b.c" << 0)));
}

TEST(WildcardProjectionValidation, OverlappingFields_InclusionAndExclusion) {
    ASSERT_NOT_OK(validateWildcardProjection(
        BSON("$**" << 1), BSON("a.b" << 0 << "e" << 1 << "d" << 0 << "a.b.c" << 0)));
    ASSERT_NOT_OK(validateWildcardProjection(
        BSON("$**" << 1), BSON("_id" << 1 << "a.b" << 0 << "e" << 1 << "d" << 0 << "a.b.c" << 0)));
}

TEST(WildcardProjectionValidation, ProjectionWithNestedDocuments) {
    ASSERT_OK(validateWildcardProjection(BSON("$**" << 1 << "d" << 1),
                                         BSON("a" << BSON("b" << 1 << "c" << 1))));
    ASSERT_OK(
        validateWildcardProjection(BSON("$**" << 1), BSON("a" << BSON("b" << 1 << "c" << 1))));
}

TEST(WildcardProjectionValidation, IdField) {
    ASSERT_OK(
        validateWildcardProjection(BSON("$**" << 1 << "other" << 1), BSON("_id" << 0 << "a" << 1)));
    ASSERT_OK(validateWildcardProjection(BSON("$**" << 1 << "other" << 1),
                                         BSON("_id" << 1 << "a" << 0 << "other" << 0)));
}
}  // namespace mongo
