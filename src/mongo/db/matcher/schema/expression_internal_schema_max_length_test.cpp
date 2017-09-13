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

TEST(InternalSchemaMaxLengthMatchExpression, TreatsMultiByteCodepointAsOneCharacter) {
    InternalSchemaMaxLengthMatchExpression nonMatchingMaxLength;
    InternalSchemaMaxLengthMatchExpression matchingMaxLength;

    ASSERT_OK(nonMatchingMaxLength.init("a", 0));
    ASSERT_OK(matchingMaxLength.init("a", 1));

    // This string has one code point, so it should meet maximum length 1 but not maximum length 0.
    constexpr auto testString = u8"\U0001f4a9";
    ASSERT_FALSE(nonMatchingMaxLength.matchesBSON(BSON("a" << testString)));
    ASSERT_TRUE(matchingMaxLength.matchesBSON(BSON("a" << testString)));
}

TEST(InternalSchemaMaxLengthMatchExpression, CorectlyCountsUnicodeCodepoints) {
    InternalSchemaMaxLengthMatchExpression nonMatchingMaxLength;
    InternalSchemaMaxLengthMatchExpression matchingMaxLength;

    ASSERT_OK(nonMatchingMaxLength.init("a", 4));
    ASSERT_OK(matchingMaxLength.init("a", 5));

    // A test string that contains single-byte, 2-byte, 3-byte, and 4-byte codepoints.
    constexpr auto testString =
        u8":"            // Single-byte character
        u8"\u00e9"       // 2-byte character
        u8")"            // Single-byte character
        u8"\U0001f4a9"   // 4-byte character
        u8"\U000020ac";  // 3-byte character

    // This string has five code points, so it should meet maximum length 5 but not maximum
    // length 4.
    ASSERT_FALSE(nonMatchingMaxLength.matchesBSON(BSON("a" << testString)));
    ASSERT_TRUE(matchingMaxLength.matchesBSON(BSON("a" << testString)));
}

TEST(InternalSchemaMaxLengthMatchExpression, DealsWithInvalidUTF8) {
    InternalSchemaMaxLengthMatchExpression maxLength;

    ASSERT_OK(maxLength.init("a", 1));

    // Several kinds of invalid byte sequences listed in the Wikipedia article about UTF-8:
    // https://en.wikipedia.org/wiki/UTF-8
    constexpr auto testStringUnexpectedContinuationByte = "\bf";
    constexpr auto testStringOverlongEncoding = "\xf0\x82\x82\xac";
    constexpr auto testStringInvalidCodePoint = "\xed\xa0\x80";  // U+d800 is not allowed
    constexpr auto testStringLeadingByteWithoutContinuationByte = "\xdf";

    // Because these inputs are invalid, we don't have any expectations about the answers we get.
    // Our only requirement is that the test does not crash.
    std::ignore = maxLength.matchesBSON(BSON("a" << testStringUnexpectedContinuationByte));
    std::ignore = maxLength.matchesBSON(BSON("a" << testStringOverlongEncoding));
    std::ignore = maxLength.matchesBSON(BSON("a" << testStringInvalidCodePoint));
    std::ignore = maxLength.matchesBSON(BSON("a" << testStringLeadingByteWithoutContinuationByte));
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
