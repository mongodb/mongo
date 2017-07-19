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

TEST(InternalSchemaMinPropertiesMatchExpression, RejectsObjectsWithTooFewElements) {
    InternalSchemaMinPropertiesMatchExpression minProperties;
    ASSERT_OK(minProperties.init(2));

    ASSERT_FALSE(minProperties.matchesBSON(BSONObj()));
    ASSERT_FALSE(minProperties.matchesBSON(BSON("b" << 21)));
}

TEST(InternalSchemaMinPropertiesMatchExpression, AcceptsObjectWithAtLeastMinElements) {
    InternalSchemaMinPropertiesMatchExpression minProperties;
    ASSERT_OK(minProperties.init(2));

    ASSERT_TRUE(minProperties.matchesBSON(BSON("b" << 21 << "c" << BSONNULL)));
    ASSERT_TRUE(minProperties.matchesBSON(BSON("b" << 21 << "c" << 3)));
    ASSERT_TRUE(minProperties.matchesBSON(BSON("b" << 21 << "c" << 3 << "d" << 43)));
}

TEST(InternalSchemaMinPropertiesMatchExpression, MinPropertiesZeroAllowsEmptyObjects) {
    InternalSchemaMinPropertiesMatchExpression minProperties;
    ASSERT_OK(minProperties.init(0));

    ASSERT_TRUE(minProperties.matchesBSON(BSONObj()));
}

TEST(InternalSchemaMinPropertiesMatchExpression, NestedObjectsAreNotUnwound) {
    InternalSchemaMinPropertiesMatchExpression minProperties;
    ASSERT_OK(minProperties.init(2));

    ASSERT_FALSE(minProperties.matchesBSON(BSON("b" << BSON("c" << 2 << "d" << 3))));
}

TEST(InternalSchemaMinPropertiesMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMinPropertiesMatchExpression minProperties;
    ASSERT_OK(minProperties.init(2));

    ASSERT_FALSE(minProperties.matchesBSON(BSON("a" << (BSON("b" << 2 << "c" << 3 << "d" << 4)))));
}

TEST(InternalSchemaMinPropertiesMatchExpression, EquivalentFunctionIsAccurate) {
    InternalSchemaMinPropertiesMatchExpression minProperties1;
    InternalSchemaMinPropertiesMatchExpression minProperties2;
    InternalSchemaMinPropertiesMatchExpression minProperties3;
    ASSERT_OK(minProperties1.init(1));
    ASSERT_OK(minProperties2.init(1));
    ASSERT_OK(minProperties3.init(2));

    ASSERT_TRUE(minProperties1.equivalent(&minProperties1));
    ASSERT_TRUE(minProperties1.equivalent(&minProperties2));
    ASSERT_FALSE(minProperties1.equivalent(&minProperties3));
}

}  // namespace
}  // namespace mongo
