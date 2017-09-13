/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(InternalSchemaMaxPropertiesMatchExpression, RejectsObjectsWithTooManyElements) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties;
    ASSERT_OK(maxProperties.init(0));

    ASSERT_FALSE(maxProperties.matchesBSON(BSON("b" << 21)));
    ASSERT_FALSE(maxProperties.matchesBSON(BSON("b" << 21 << "c" << 3)));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, AcceptsObjectWithLessThanOrEqualToMaxElements) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties;
    ASSERT_OK(maxProperties.init(2));

    ASSERT_TRUE(maxProperties.matchesBSON(BSONObj()));
    ASSERT_TRUE(maxProperties.matchesBSON(BSON("b" << BSONNULL)));
    ASSERT_TRUE(maxProperties.matchesBSON(BSON("b" << 21)));
    ASSERT_TRUE(maxProperties.matchesBSON(BSON("b" << 21 << "c" << 3)));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, MaxPropertiesZeroAllowsEmptyObjects) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties;
    ASSERT_OK(maxProperties.init(0));

    ASSERT_TRUE(maxProperties.matchesBSON(BSONObj()));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, NestedObjectsAreNotUnwound) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties;
    ASSERT_OK(maxProperties.init(1));

    ASSERT_TRUE(maxProperties.matchesBSON(BSON("b" << BSON("c" << 2 << "d" << 3))));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, EquivalentFunctionIsAccurate) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties1;
    InternalSchemaMaxPropertiesMatchExpression maxProperties2;
    InternalSchemaMaxPropertiesMatchExpression maxProperties3;
    ASSERT_OK(maxProperties1.init(1));
    ASSERT_OK(maxProperties2.init(1));
    ASSERT_OK(maxProperties3.init(2));

    ASSERT_TRUE(maxProperties1.equivalent(&maxProperties1));
    ASSERT_TRUE(maxProperties1.equivalent(&maxProperties2));
    ASSERT_FALSE(maxProperties1.equivalent(&maxProperties3));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties;
    ASSERT_OK(maxProperties.init(2));

    ASSERT_TRUE(maxProperties.matchesBSON(BSON("a" << (BSON("b" << 2 << "c" << 3 << "d" << 4)))));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, MinPropertiesNotEquivalentToMaxProperties) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties;
    InternalSchemaMinPropertiesMatchExpression minProperties;
    ASSERT_OK(maxProperties.init(5));
    ASSERT_OK(minProperties.init(5));

    ASSERT_FALSE(maxProperties.equivalent(&minProperties));
}

}  // namespace
}  // namespace mongo
