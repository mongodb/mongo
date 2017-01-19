/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/field_path.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using std::string;
using std::vector;

/** FieldPath constructed from empty string. */
TEST(FieldPathTest, Empty) {
    ASSERT_THROWS(FieldPath path(""), UserException);
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
    ASSERT_THROWS(FieldPath path("$"), UserException);
}

/** FieldPath with a '$' prefix. */
TEST(FieldPathTest, DollarSignPrefix) {
    ASSERT_THROWS(FieldPath path("$a"), UserException);
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
    ASSERT_THROWS(FieldPath path("a.$b"), UserException);
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
    ASSERT_THROWS(FieldPath path("foo."), UserException);
}

/** FieldPath constructed from a string beginning with a dot. */
TEST(FieldPathTest, PrefixDot) {
    ASSERT_THROWS(FieldPath path(".foo"), UserException);
}

/** FieldPath constructed from a string with adjacent dots. */
TEST(FieldPathTest, AdjacentDots) {
    ASSERT_THROWS(FieldPath path("foo..bar"), UserException);
}

/** FieldPath constructed with only dots. */
TEST(FieldPathTest, OnlyDots) {
    ASSERT_THROWS(FieldPath path("..."), UserException);
}

/** FieldPath constructed from a string with one letter between two dots. */
TEST(FieldPathTest, LetterBetweenDots) {
    FieldPath path("foo.a.bar");
    ASSERT_EQUALS(3U, path.getPathLength());
    ASSERT_EQUALS("foo.a.bar", path.fullPath());
}

/** FieldPath containing a null character. */
TEST(FieldPathTest, NullCharacter) {
    ASSERT_THROWS(FieldPath path(string("foo.b\0r", 7)), UserException);
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

}  // namespace
}  // namespace mongo
