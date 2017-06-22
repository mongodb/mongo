/**
 * Copyright 2017 (c) 10gen Inc.
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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(InternalSchemaMaxLengthMatchExpression, RejectsNonStringElements) {
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(maxLength.init("a", 1));

    ASSERT_FALSE(maxLength.matchesBSON(BSON("a" << BSONObj())));
    ASSERT_FALSE(maxLength.matchesBSON(BSON("a" << 1)));
    ASSERT_FALSE(maxLength.matchesBSON(BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaMaxLengthMatchExpression, RejectsStringsWithTooManyChars) {
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(maxLength.init("a", 2));

    ASSERT_FALSE(maxLength.matchesBSON(BSON("a"
                                            << "abc")));
    ASSERT_FALSE(maxLength.matchesBSON(BSON("a"
                                            << "abcd")));
}

TEST(InternalSchemaMaxLengthMatchExpression, AcceptsStringsWithLessThanOrEqualToMax) {
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(maxLength.init("a", 2));

    ASSERT_TRUE(maxLength.matchesBSON(BSON("a"
                                           << "ab")));
    ASSERT_TRUE(maxLength.matchesBSON(BSON("a"
                                           << "a")));
    ASSERT_TRUE(maxLength.matchesBSON(BSON("a"
                                           << "")));
}

TEST(InternalSchemaMaxLengthMatchExpression, MaxLengthZeroAllowsEmptyString) {
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(maxLength.init("a", 0));

    ASSERT_TRUE(maxLength.matchesBSON(BSON("a"
                                           << "")));
}

TEST(InternalSchemaMaxLengthMatchExpression, RejectsNull) {
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(maxLength.init("a", 1));

    ASSERT_FALSE(maxLength.matchesBSON(BSON("a" << BSONNULL)));
}

TEST(InternalSchemaMaxLengthMatchExpression, NestedArraysWorkWithDottedPaths) {
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(maxLength.init("a.b", 2));

    ASSERT_TRUE(maxLength.matchesBSON(BSON("a" << BSON("b"
                                                       << "a"))));
    ASSERT_TRUE(maxLength.matchesBSON(BSON("a" << BSON("b"
                                                       << "ab"))));
    ASSERT_FALSE(maxLength.matchesBSON(BSON("a" << BSON("b"
                                                        << "abc"))));
}

TEST(InternalSchemaMaxLengthMatchExpression, SameMaxLengthTreatedEquivalent) {
    InternalSchemaMaxLengthMatchExpression maxLength1;
    InternalSchemaMaxLengthMatchExpression maxLength2;
    InternalSchemaMaxLengthMatchExpression maxLength3;
    ASSERT_OK(maxLength1.init("a", 2));
    ASSERT_OK(maxLength2.init("a", 2));
    ASSERT_OK(maxLength3.init("a", 3));

    ASSERT_TRUE(maxLength1.equivalent(&maxLength2));
    ASSERT_FALSE(maxLength1.equivalent(&maxLength3));
}

TEST(InternalSchemaMaxLengthMatchExpression, MinLengthAndMaxLengthAreNotEquivalent) {
    InternalSchemaMinLengthMatchExpression minLength;
    InternalSchemaMaxLengthMatchExpression maxLength;
    ASSERT_OK(minLength.init("a", 2));
    ASSERT_OK(maxLength.init("a", 2));

    ASSERT_FALSE(maxLength.equivalent(&minLength));
}

}  // namespace
}  // namespace mongo
