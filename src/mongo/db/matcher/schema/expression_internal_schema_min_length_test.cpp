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
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(InternalSchemaMinLengthMatchExpression, RejectsNonStringElements) {
    InternalSchemaMinLengthMatchExpression minLength;
    ASSERT_OK(minLength.init("a", 1));

    ASSERT_FALSE(minLength.matchesBSON(BSON("a" << BSONObj())));
    ASSERT_FALSE(minLength.matchesBSON(BSON("a" << 1)));
    ASSERT_FALSE(minLength.matchesBSON(BSON("a" << BSON_ARRAY(1))));
}

TEST(InternalSchemaMinLengthMatchExpression, RejectsStringsWithTooFewChars) {
    InternalSchemaMinLengthMatchExpression minLength;
    ASSERT_OK(minLength.init("a", 2));

    ASSERT_FALSE(minLength.matchesBSON(BSON("a"
                                            << "")));
    ASSERT_FALSE(minLength.matchesBSON(BSON("a"
                                            << "a")));
}

TEST(InternalSchemaMinLengthMatchExpression, AcceptsStringWithAtLeastMinChars) {
    InternalSchemaMinLengthMatchExpression minLength;
    ASSERT_OK(minLength.init("a", 2));

    ASSERT_TRUE(minLength.matchesBSON(BSON("a"
                                           << "ab")));
    ASSERT_TRUE(minLength.matchesBSON(BSON("a"
                                           << "abc")));
    ASSERT_TRUE(minLength.matchesBSON(BSON("a"
                                           << "abcde")));
}

TEST(InternalSchemaMinLengthMatchExpression, MinLengthZeroAllowsEmptyString) {
    InternalSchemaMinLengthMatchExpression minLength;
    ASSERT_OK(minLength.init("a", 0));

    ASSERT_TRUE(minLength.matchesBSON(BSON("a"
                                           << "")));
}

TEST(InternalSchemaMinLengthMatchExpression, RejectsNull) {
    InternalSchemaMinLengthMatchExpression minLength;
    ASSERT_OK(minLength.init("a", 1));

    ASSERT_FALSE(minLength.matchesBSON(BSON("a" << BSONNULL)));
}

TEST(InternalSchemaMinLengthMatchExpression, TreatsMultiByteCodepointAsOneCharacter) {
    InternalSchemaMinLengthMatchExpression matchingMinLength;
    InternalSchemaMinLengthMatchExpression nonMatchingMinLength;

    ASSERT_OK(matchingMinLength.init("a", 1));
    ASSERT_OK(nonMatchingMinLength.init("a", 2));

    // This string has one code point, so it should meet minimum length 1 but not minimum length 2.
    constexpr auto testString = u8"\U0001f4a9";
    ASSERT_TRUE(matchingMinLength.matchesBSON(BSON("a" << testString)));
    ASSERT_FALSE(nonMatchingMinLength.matchesBSON(BSON("a" << testString)));
}

TEST(InternalSchemaMinLengthMatchExpression, CorectlyCountsUnicodeCodepoints) {
    InternalSchemaMinLengthMatchExpression matchingMinLength;
    InternalSchemaMinLengthMatchExpression nonMatchingMinLength;

    ASSERT_OK(matchingMinLength.init("a", 5));
    ASSERT_OK(nonMatchingMinLength.init("a", 6));

    // A test string that contains single-byte, 2-byte, 3-byte, and 4-byte code points.
    constexpr auto testString =
        u8":"            // Single-byte character
        u8"\u00e9"       // 2-byte character
        u8")"            // Single-byte character
        u8"\U0001f4a9"   // 4-byte character
        u8"\U000020ac";  // 3-byte character

    // This string has five code points, so it should meet minimum length 5 but not minimum
    // length 6.
    ASSERT_TRUE(matchingMinLength.matchesBSON(BSON("a" << testString)));
    ASSERT_FALSE(nonMatchingMinLength.matchesBSON(BSON("a" << testString)));
}

TEST(InternalSchemaMinLengthMatchExpression, DealsWithInvalidUTF8) {
    InternalSchemaMinLengthMatchExpression minLength;

    ASSERT_OK(minLength.init("a", 1));

    // Several kinds of invalid byte sequences listed in the Wikipedia article about UTF-8:
    // https://en.wikipedia.org/wiki/UTF-8
    constexpr auto testStringUnexpectedContinuationByte = "\bf";
    constexpr auto testStringOverlongEncoding = "\xf0\x82\x82\xac";
    constexpr auto testStringInvalidCodePoint = "\xed\xa0\x80";  // U+d800 is not allowed
    constexpr auto testStringLeadingByteWithoutContinuationByte = "\xdf";

    // Because these inputs are invalid, we don't have any expectations about the answers we get.
    // Our only requirement is that the test does not crash.
    std::ignore = minLength.matchesBSON(BSON("a" << testStringUnexpectedContinuationByte));
    std::ignore = minLength.matchesBSON(BSON("a" << testStringOverlongEncoding));
    std::ignore = minLength.matchesBSON(BSON("a" << testStringInvalidCodePoint));
    std::ignore = minLength.matchesBSON(BSON("a" << testStringLeadingByteWithoutContinuationByte));
}

TEST(InternalSchemaMinLengthMatchExpression, NestedFieldsWorkWithDottedPaths) {
    InternalSchemaMinLengthMatchExpression minLength;
    ASSERT_OK(minLength.init("a.b", 2));

    ASSERT_TRUE(minLength.matchesBSON(BSON("a" << BSON("b"
                                                       << "ab"))));
    ASSERT_TRUE(minLength.matchesBSON(BSON("a" << BSON("b"
                                                       << "abc"))));
    ASSERT_FALSE(minLength.matchesBSON(BSON("a" << BSON("b"
                                                        << "a"))));
}

TEST(InternalSchemaMinLengthMatchExpression, SameMinLengthTreatedEquivalent) {
    InternalSchemaMinLengthMatchExpression minLength1;
    InternalSchemaMinLengthMatchExpression minLength2;
    InternalSchemaMinLengthMatchExpression minLength3;
    ASSERT_OK(minLength1.init("a", 2));
    ASSERT_OK(minLength2.init("a", 2));
    ASSERT_OK(minLength3.init("a", 3));

    ASSERT_TRUE(minLength1.equivalent(&minLength2));
    ASSERT_FALSE(minLength1.equivalent(&minLength3));
}

}  // namespace
}  // namespace mongo
