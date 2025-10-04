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

#include "mongo/db/pipeline/field_path.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using std::string;

/** FieldPath constructed from empty string. */
TEST(FieldPathTest, Empty) {
    ASSERT_THROWS(FieldPath(""), AssertionException);
}

/** FieldPath constructed from a simple string (without dots). */
TEST(FieldPathTest, Simple) {
    FieldPath path("foo");
    ASSERT_EQUALS(1U, path.getPathLength());
    ASSERT_EQUALS("foo", path.getFieldName(0));
    ASSERT_EQUALS("foo", path.fullPath());
    ASSERT_EQUALS("$foo", path.fullPathWithPrefix());
}

/** FieldPath consisting of a '$' character. */
TEST(FieldPathTest, DollarSign) {
    ASSERT_THROWS(FieldPath("$"), AssertionException);
}

/** FieldPath with a '$' prefix. */
TEST(FieldPathTest, DollarSignPrefix) {
    ASSERT_THROWS(FieldPath("$a"), AssertionException);
}

/** FieldPath constructed from a string with one dot. */
TEST(FieldPathTest, Dotted) {
    FieldPath path("foo.bar");
    ASSERT_EQUALS(2U, path.getPathLength());
    ASSERT_EQUALS("foo", path.getFieldName(0));
    ASSERT_EQUALS("bar", path.getFieldName(1));
    ASSERT_EQUALS("foo.bar", path.fullPath());
    ASSERT_EQUALS("$foo.bar", path.fullPathWithPrefix());
}

/** FieldPath with a '$' prefix in the second field. */
TEST(FieldPathTest, DollarSignPrefixSecondField) {
    ASSERT_THROWS(FieldPath("a.$b"), AssertionException);
}

/** FieldPath constructed from a string with two dots. */
TEST(FieldPathTest, TwoDotted) {
    FieldPath path("foo.bar.baz");
    ASSERT_EQUALS(3U, path.getPathLength());
    ASSERT_EQUALS("foo", path.getFieldName(0));
    ASSERT_EQUALS("bar", path.getFieldName(1));
    ASSERT_EQUALS("baz", path.getFieldName(2));
    ASSERT_EQUALS("foo.bar.baz", path.fullPath());
}

/** FieldPath constructed from a string ending in a dot. */
TEST(FieldPathTest, TerminalDot) {
    ASSERT_THROWS(FieldPath("foo."), AssertionException);
}

/** FieldPath constructed from a string beginning with a dot. */
TEST(FieldPathTest, PrefixDot) {
    ASSERT_THROWS(FieldPath(".foo"), AssertionException);
}

/** FieldPath constructed from a string with adjacent dots. */
TEST(FieldPathTest, AdjacentDots) {
    ASSERT_THROWS(FieldPath("foo..bar"), AssertionException);
}

/** FieldPath constructed with only dots. */
TEST(FieldPathTest, OnlyDots) {
    ASSERT_THROWS(FieldPath("..."), AssertionException);
}

/** FieldPath constructed from a string with one letter between two dots. */
TEST(FieldPathTest, LetterBetweenDots) {
    FieldPath path("foo.a.bar");
    ASSERT_EQUALS(3U, path.getPathLength());
    ASSERT_EQUALS("foo.a.bar", path.fullPath());
}

/** FieldPath containing a null character. */
TEST(FieldPathTest, NullCharacter) {
    ASSERT_THROWS(FieldPath(string("foo.b\0r", 7)), AssertionException);
}

/** Tail of a FieldPath. */
TEST(FieldPathTest, Tail) {
    FieldPath path = FieldPath("foo.bar").tail();
    ASSERT_EQUALS(1U, path.getPathLength());
    ASSERT_EQUALS("bar", path.fullPath());
}

/** Tail of a FieldPath with three fields. */
TEST(FieldPathTest, TailThreeFields) {
    FieldPath path = FieldPath("foo.bar.baz").tail();
    ASSERT_EQUALS(2U, path.getPathLength());
    ASSERT_EQUALS("bar.baz", path.fullPath());
}

/**
 * Creates a FieldPath that represents a document nested 'depth' levels deep.
 */
FieldPath makeFieldPathOfDepth(size_t depth) {
    StringBuilder builder;
    builder << "a";
    for (size_t i = 0; i < depth - 1; i++) {
        builder << ".a";
    }
    return FieldPath(builder.str());
}

// Tests that long field paths at or under the depth limit can be created successfully.
TEST(FieldPathTest, CanConstructFieldPathAtOrUnderDepthLimit) {
    ASSERT_EQUALS(makeFieldPathOfDepth(BSONDepth::getMaxAllowableDepth() - 1).getPathLength(),
                  BSONDepth::getMaxAllowableDepth() - 1);
    ASSERT_EQUALS(makeFieldPathOfDepth(BSONDepth::getMaxAllowableDepth()).getPathLength(),
                  BSONDepth::getMaxAllowableDepth());
}

// Tests that a FieldPath can't be constructed if the path is too deeply nested.
TEST(FieldPathTest, ConstructorAssertsOnDeeplyNestedPath) {
    ASSERT_THROWS_CODE(FieldPath(makeFieldPathOfDepth(BSONDepth::getMaxAllowableDepth() + 1)),
                       AssertionException,
                       ErrorCodes::Overflow);
}

/**
 * Creates a FieldPath that represents an array nested 'depth' levels deep.
 */
FieldPath makeArrayFieldPathOfDepth(size_t depth) {
    StringBuilder builder;
    builder << "a";
    for (size_t i = 0; i < depth - 1; i++) {
        builder << ".0";
    }
    return FieldPath(builder.str());
}

// Tests that long array field paths at or under the depth limit can be created successfully.
TEST(FieldPathTest, CanConstructArrayFieldPathAtOrUnderDepthLimit) {
    ASSERT_EQUALS(makeArrayFieldPathOfDepth(BSONDepth::getMaxAllowableDepth() - 1).getPathLength(),
                  BSONDepth::getMaxAllowableDepth() - 1);
    ASSERT_EQUALS(makeArrayFieldPathOfDepth(BSONDepth::getMaxAllowableDepth()).getPathLength(),
                  BSONDepth::getMaxAllowableDepth());
}

// Tests that a FieldPath can't be constructed if an array path is too deeply nested.
TEST(FieldPathTest, ConstructorAssertsOnDeeplyNestedArrayPath) {
    ASSERT_THROWS_CODE(makeArrayFieldPathOfDepth(BSONDepth::getMaxAllowableDepth() + 1),
                       AssertionException,
                       ErrorCodes::Overflow);
}

// Test FieldPath::getSubpath().
TEST(FieldPathTest, GetSubpath) {
    FieldPath path = FieldPath("foo.bar.baz");
    ASSERT_EQUALS("foo", path.getSubpath(0));
    ASSERT_EQUALS("foo.bar", path.getSubpath(1));
    ASSERT_EQUALS("foo.bar.baz", path.getSubpath(2));
}

void checkConcatWorks(const FieldPath& head, const FieldPath& tail) {
    FieldPath concat = head.concat(tail);
    ASSERT(concat == FieldPath::getFullyQualifiedPath(head.fullPath(), tail.fullPath()));
    ASSERT_EQ(concat.getPathLength(), head.getPathLength() + tail.getPathLength());

    const auto expectedTail = head.getPathLength() == 1
        ? tail
        : FieldPath::getFullyQualifiedPath(head.tail().fullPath(), tail.fullPath());
    ASSERT(FieldPath(concat.tail()) == expectedTail);

    ASSERT_EQ(concat.front(), head.front());
    ASSERT_EQ(concat.back(), tail.back());
    for (size_t i = 0; i < concat.getPathLength(); i++) {
        const auto expected = (i < head.getPathLength())
            ? head.getFieldName(i)
            : tail.getFieldName(i - head.getPathLength());
        ASSERT_EQ(concat.getFieldName(i), expected);
    }
}

TEST(FieldPathTest, Concat) {
    checkConcatWorks("abc", "cde");
    checkConcatWorks("abc.ef", "cde.ab");
    checkConcatWorks("abc.$id", "cde");
    checkConcatWorks("abc", "$id.x");
    checkConcatWorks("some.long.path.with.many.parts", "another.long.ish.path");
    checkConcatWorks("$db", "$id");
    checkConcatWorks("$db.$id", "$id.$db");
}

TEST(FieldPathTest, ConcatFailsIfExceedsMaxDepth) {
    std::string firstHalfStr;
    std::string secondHalfStr;
    int pathLength = 201;
    int firstHalfMax = 99;
    int secondHalfMax = pathLength - 1;
    for (int i = 0; i <= firstHalfMax; ++i) {
        firstHalfStr.append(std::to_string(pathLength - i));
        firstHalfStr.append(".");
    }
    firstHalfStr.append(std::to_string(firstHalfMax + 1));
    for (int i = 101; i < secondHalfMax; ++i) {
        secondHalfStr.append(std::to_string(pathLength - i));
        secondHalfStr.append(".");
    }
    secondHalfStr.append(std::to_string(pathLength));
    FieldPath firstHalf(firstHalfStr);
    FieldPath secondHalf(secondHalfStr);
    ASSERT_THROWS_CODE(firstHalf.concat(secondHalf), AssertionException, ErrorCodes::Overflow);
}

TEST(FieldPathTest, SubstractPrefix) {
    const FieldPath fp{"first.second.third"};
    ASSERT_EQ(fp.subtractPrefix(2).fullPath(), std::string("third"));
    ASSERT_EQ(fp.subtractPrefix(1).fullPath(), std::string("second.third"));
    ASSERT_EQ(fp.subtractPrefix(0).fullPath(), std::string("first.second.third"));
}

DEATH_TEST_REGEX(FieldPathTest, SubstractPrefix_TooLargeCut, "Tripwire assertion.*10985000") {
    FieldPath("first.second.third").subtractPrefix(3);
}
}  // namespace
}  // namespace mongo
