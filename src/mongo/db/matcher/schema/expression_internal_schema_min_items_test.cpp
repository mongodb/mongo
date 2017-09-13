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
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(InternalSchemaMinItemsMatchExpression, RejectsNonArrayElements) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a", 1));

    ASSERT(!minItems.matchesBSON(BSON("a" << BSONObj())));
    ASSERT(!minItems.matchesBSON(BSON("a" << 1)));
    ASSERT(!minItems.matchesBSON(BSON("a"
                                      << "string")));
}

TEST(InternalSchemaMinItemsMatchExpression, RejectsArraysWithTooFewElements) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a", 2));

    ASSERT(!minItems.matchesBSON(BSON("a" << BSONArray())));
    ASSERT(!minItems.matchesBSON(BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaMinItemsMatchExpression, AcceptsArrayWithAtLeastMinElements) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a", 2));

    ASSERT(minItems.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2))));
    ASSERT(minItems.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(InternalSchemaMinItemsMatchExpression, MinItemsZeroAllowsEmptyArrays) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a", 0));

    ASSERT(minItems.matchesBSON(BSON("a" << BSONArray())));
}

TEST(InternalSchemaMinItemsMatchExpression, NullArrayEntriesCountAsItems) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a", 1));

    ASSERT(minItems.matchesBSON(BSON("a" << BSON_ARRAY(BSONNULL))));
    ASSERT(minItems.matchesBSON(BSON("a" << BSON_ARRAY(BSONNULL << 1))));
}

TEST(InternalSchemaMinItemsMatchExpression, NestedArraysAreNotUnwound) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a", 2));

    ASSERT(!minItems.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(1 << 2)))));
}

TEST(InternalSchemaMinItemsMatchExpression, NestedArraysWorkWithDottedPaths) {
    InternalSchemaMinItemsMatchExpression minItems;
    ASSERT_OK(minItems.init("a.b", 2));

    ASSERT(minItems.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(1 << 2)))));
    ASSERT(!minItems.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(1)))));
}

}  // namespace
}  // namespace mongo
