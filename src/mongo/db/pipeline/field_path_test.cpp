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

#include "mongo/bson/bson_depth.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
using std::string;
using std::vector;

/** FieldPath constructed from empty string. */
class Empty {
public:
    void run() {
        ASSERT_THROWS(FieldPath path(""), UserException);
    }
};

/** FieldPath constructed from a simple string (without dots). */
class Simple {
public:
    void run() {
        FieldPath path("foo");
        ASSERT_EQUALS(1U, path.getPathLength());
        ASSERT_EQUALS("foo", path.getFieldName(0));
        ASSERT_EQUALS("foo", path.fullPath());
        ASSERT_EQUALS("$foo", path.fullPathWithPrefix());
    }
};

/** FieldPath consisting of a '$' character. */
class DollarSign {
public:
    void run() {
        ASSERT_THROWS(FieldPath path("$"), UserException);
    }
};

/** FieldPath with a '$' prefix. */
class DollarSignPrefix {
public:
    void run() {
        ASSERT_THROWS(FieldPath path("$a"), UserException);
    }
};

/** FieldPath constructed from a string with one dot. */
class Dotted {
public:
    void run() {
        FieldPath path("foo.bar");
        ASSERT_EQUALS(2U, path.getPathLength());
        ASSERT_EQUALS("foo", path.getFieldName(0));
        ASSERT_EQUALS("bar", path.getFieldName(1));
        ASSERT_EQUALS("foo.bar", path.fullPath());
        ASSERT_EQUALS("$foo.bar", path.fullPathWithPrefix());
    }
};

/** FieldPath with a '$' prefix in the second field. */
class DollarSignPrefixSecondField {
public:
    void run() {
        ASSERT_THROWS(FieldPath path("a.$b"), UserException);
    }
};

/** FieldPath constructed from a string with two dots. */
class TwoDotted {
public:
    void run() {
        FieldPath path("foo.bar.baz");
        ASSERT_EQUALS(3U, path.getPathLength());
        ASSERT_EQUALS("foo", path.getFieldName(0));
        ASSERT_EQUALS("bar", path.getFieldName(1));
        ASSERT_EQUALS("baz", path.getFieldName(2));
        ASSERT_EQUALS("foo.bar.baz", path.fullPath());
    }
};

/** FieldPath constructed from a string ending in a dot. */
class TerminalDot {
public:
    void run() {
        ASSERT_THROWS(FieldPath path("foo."), UserException);
    }
};

/** FieldPath constructed from a string beginning with a dot. */
class PrefixDot {
public:
    void run() {
        ASSERT_THROWS(FieldPath path(".foo"), UserException);
    }
};

/** FieldPath constructed from a string with adjacent dots. */
class AdjacentDots {
public:
    void run() {
        ASSERT_THROWS(FieldPath path("foo..bar"), UserException);
    }
};

/** FieldPath constructed with only dots. */
class OnlyDots {
public:
    void run() {
        ASSERT_THROWS(FieldPath path("..."), UserException);
    }
};

/** FieldPath constructed from a string with one letter between two dots. */
class LetterBetweenDots {
public:
    void run() {
        FieldPath path("foo.a.bar");
        ASSERT_EQUALS(3U, path.getPathLength());
        ASSERT_EQUALS("foo.a.bar", path.fullPath());
    }
};

/** FieldPath containing a null character. */
class NullCharacter {
public:
    void run() {
        ASSERT_THROWS(FieldPath path(string("foo.b\0r", 7)), UserException);
    }
};

/** Tail of a FieldPath. */
class Tail {
public:
    void run() {
        FieldPath path = FieldPath("foo.bar").tail();
        ASSERT_EQUALS(1U, path.getPathLength());
        ASSERT_EQUALS("bar", path.fullPath());
    }
};

/** Tail of a FieldPath with three fields. */
class TailThreeFields {
public:
    void run() {
        FieldPath path = FieldPath("foo.bar.baz").tail();
        ASSERT_EQUALS(2U, path.getPathLength());
        ASSERT_EQUALS("bar.baz", path.fullPath());
    }
};

class All : public Suite {
public:
    All() : Suite("field_path") {}
    void setupTests() {
        add<Empty>();
        add<Simple>();
        add<DollarSign>();
        add<DollarSignPrefix>();
        add<Dotted>();
        add<DollarSignPrefixSecondField>();
        add<TwoDotted>();
        add<TerminalDot>();
        add<PrefixDot>();
        add<AdjacentDots>();
        add<OnlyDots>();
        add<LetterBetweenDots>();
        add<NullCharacter>();
        add<Tail>();
        add<TailThreeFields>();
    }
};
SuiteInstance<All> myall;

namespace {
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
                       UserException,
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
                       UserException,
                       ErrorCodes::Overflow);
}
}  // namespace
}  // namespace mongo
