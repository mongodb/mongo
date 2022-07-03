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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(InternalSchemaMinPropertiesMatchExpression, RejectsObjectsWithTooFewElements) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_FALSE(minProperties.matchesBSON(BSONObj()));
    ASSERT_FALSE(minProperties.matchesBSON(BSON("b" << 21)));
}

TEST(InternalSchemaMinPropertiesMatchExpression, AcceptsObjectWithAtLeastMinElements) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_TRUE(minProperties.matchesBSON(BSON("b" << 21 << "c" << BSONNULL)));
    ASSERT_TRUE(minProperties.matchesBSON(BSON("b" << 21 << "c" << 3)));
    ASSERT_TRUE(minProperties.matchesBSON(BSON("b" << 21 << "c" << 3 << "d" << 43)));
}

TEST(InternalSchemaMinPropertiesMatchExpression, MinPropertiesZeroAllowsEmptyObjects) {
    InternalSchemaMinPropertiesMatchExpression minProperties(0);

    ASSERT_TRUE(minProperties.matchesBSON(BSONObj()));
}

TEST(InternalSchemaMinPropertiesMatchExpression, NestedObjectsAreNotUnwound) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_FALSE(minProperties.matchesBSON(BSON("b" << BSON("c" << 2 << "d" << 3))));
}

TEST(InternalSchemaMinPropertiesMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMinPropertiesMatchExpression minProperties(2);

    ASSERT_FALSE(minProperties.matchesBSON(BSON("a" << (BSON("b" << 2 << "c" << 3 << "d" << 4)))));
}

TEST(InternalSchemaMinPropertiesMatchExpression, EquivalentFunctionIsAccurate) {
    InternalSchemaMinPropertiesMatchExpression minProperties1(1);
    InternalSchemaMinPropertiesMatchExpression minProperties2(1);
    InternalSchemaMinPropertiesMatchExpression minProperties3(2);

    ASSERT_TRUE(minProperties1.equivalent(&minProperties1));
    ASSERT_TRUE(minProperties1.equivalent(&minProperties2));
    ASSERT_FALSE(minProperties1.equivalent(&minProperties3));
}

DEATH_TEST_REGEX(InternalSchemaMinPropertiesMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400216") {
    InternalSchemaMaxPropertiesMatchExpression minProperties(1);

    ASSERT_EQ(minProperties.numChildren(), 0);
    ASSERT_THROWS_CODE(minProperties.getChild(0), AssertionException, 6400216);
}

}  // namespace
}  // namespace mongo
