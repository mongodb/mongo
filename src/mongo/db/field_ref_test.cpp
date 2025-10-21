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

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

TEST(Empty, NoFields) {
    FieldRef fieldRef("");
    ASSERT_EQUALS(fieldRef.numParts(), 0U);
    ASSERT_EQUALS(fieldRef.dottedField(), "");
}

TEST(Empty, NoFieldNames) {
    std::string field = ".";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 2U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Empty, NoFieldNames2) {
    std::string field = "..";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 3U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "");
    ASSERT_EQUALS(fieldRef.getPart(2), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Empty, EmptyFieldName) {
    std::string field = ".b.";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 3U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "b");
    ASSERT_EQUALS(fieldRef.getPart(2), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Empty, ReinitializeWithEmptyString) {
    FieldRef fieldRef("a.b.c.d.e");
    ASSERT_EQUALS(fieldRef.numParts(), 5U);

    fieldRef.parse("");
    ASSERT_EQUALS(fieldRef.numParts(), 0U);
    ASSERT_EQUALS(fieldRef.dottedField(), "");
}

TEST(Normal, SinglePart) {
    std::string field = "a";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), field);
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Normal, ParseTwice) {
    std::string field = "a";
    FieldRef fieldRef;
    for (int i = 0; i < 2; i++) {
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 1U);
        ASSERT_EQUALS(fieldRef.getPart(0), field);
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }
}

TEST(Normal, MulitplePartsVariable) {
    const char* parts[] = {"a", "b", "c", "d", "e"};
    size_t size = sizeof(parts) / sizeof(char*);
    std::string field(parts[0]);
    for (size_t i = 1; i < size; i++) {
        field.append(1, '.');
        field.append(parts[i]);
    }

    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), size);
    for (size_t i = 0; i < size; i++) {
        ASSERT_EQUALS(fieldRef.getPart(i), parts[i]);
    }
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Replacement, SingleField) {
    std::string field = "$";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), "$");

    std::string newField = "a";
    fieldRef.setPart(0, newField);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), newField);
    ASSERT_EQUALS(fieldRef.dottedField(), newField);
}

TEST(Replacement, InMultipleField) {
    std::string field = "a.b.c.$.e";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 5U);
    ASSERT_EQUALS(fieldRef.getPart(3), "$");

    std::string newField = "d";
    fieldRef.setPart(3, newField);
    ASSERT_EQUALS(fieldRef.numParts(), 5U);
    ASSERT_EQUALS(fieldRef.getPart(3), newField);
    ASSERT_EQUALS(fieldRef.dottedField(), "a.b.c.d.e");
}

TEST(Replacement, SameFieldMultipleReplacements) {
    std::string prefix = "a.";
    std::string field = prefix + "$";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 2U);

    const char* parts[] = {"a", "b", "c", "d", "e"};
    size_t size = sizeof(parts) / sizeof(char*);
    for (size_t i = 0; i < size; i++) {
        fieldRef.setPart(1, parts[i]);
        ASSERT_EQUALS(fieldRef.dottedField(), prefix + parts[i]);
    }
}

TEST(Prefix, Normal) {
    FieldRef prefix, base("a.b.c");

    prefix.parse("a.b");
    ASSERT_TRUE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));

    prefix.parse("a");
    ASSERT_TRUE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));

    prefix.parse("a.b.c");
    ASSERT_FALSE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));
}

TEST(Prefix, Dotted) {
    FieldRef prefix("a.0"), base("a.0.c");
    ASSERT_TRUE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));

    prefix.parse("a.0.c");
    ASSERT_FALSE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));
}

TEST(Prefix, NoPrefixes) {
    FieldRef prefix("a.b"), base("a.b");
    ASSERT_FALSE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));

    base.parse("a");
    ASSERT_FALSE(prefix.isPrefixOf(base));
    ASSERT_FALSE(prefix.isPrefixOfOrEqualTo(base));

    base.parse("b");
    ASSERT_FALSE(prefix.isPrefixOf(base));
    ASSERT_FALSE(prefix.isPrefixOfOrEqualTo(base));
}

TEST(Prefix, EmptyBase) {
    FieldRef field("a"), empty;
    ASSERT_FALSE(field.isPrefixOf(empty));
    ASSERT_FALSE(empty.isPrefixOf(field));
    ASSERT_FALSE(empty.isPrefixOf(empty));
}

TEST(PrefixSize, Normal) {
    FieldRef fieldA("a.b"), fieldB("a");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 1U);

    fieldB.parse("a.b");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 2U);

    fieldB.parse("a.b.c");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 2U);
}

TEST(PrefixSize, NoCommonatility) {
    FieldRef fieldA, fieldB;
    fieldA.parse("a");
    fieldB.parse("b");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 0U);
}

TEST(PrefixSize, Empty) {
    FieldRef fieldA("a"), empty;
    ASSERT_EQUALS(fieldA.commonPrefixSize(empty), 0U);
    ASSERT_EQUALS(empty.commonPrefixSize(fieldA), 0U);
}

TEST(FullyOverlapsWith, Normal) {
    FieldRef fieldA("a"), fieldB("a.b"), fieldC("a.b.c");
    FieldRef fieldD("a.b.d");
    ASSERT(fieldA.fullyOverlapsWith(fieldA));
    ASSERT(fieldA.fullyOverlapsWith(fieldB));
    ASSERT(fieldA.fullyOverlapsWith(fieldC));
    ASSERT(fieldA.fullyOverlapsWith(fieldD));
    ASSERT(fieldB.fullyOverlapsWith(fieldA));
    ASSERT(fieldB.fullyOverlapsWith(fieldB));
    ASSERT(fieldB.fullyOverlapsWith(fieldC));
    ASSERT(fieldB.fullyOverlapsWith(fieldD));
    ASSERT(fieldC.fullyOverlapsWith(fieldA));
    ASSERT(fieldC.fullyOverlapsWith(fieldB));
    ASSERT(fieldC.fullyOverlapsWith(fieldC));

    ASSERT_FALSE(fieldD.fullyOverlapsWith(fieldC));
    ASSERT_FALSE(fieldC.fullyOverlapsWith(fieldD));
}

TEST(FullyOverlapsWith, NoCommonality) {
    FieldRef fieldA("a.b.c"), fieldB("b.c.d");
    ASSERT_FALSE(fieldA.fullyOverlapsWith(fieldB));
    ASSERT_FALSE(fieldB.fullyOverlapsWith(fieldA));
}

TEST(FullyOverlapsWith, Empty) {
    FieldRef field("a"), empty;
    ASSERT_FALSE(field.fullyOverlapsWith(empty));
    ASSERT_FALSE(empty.fullyOverlapsWith(field));
}

TEST(Equality, Simple1) {
    FieldRef a("a.b");
    ASSERT(a.equalsDottedField("a.b"));
    ASSERT(!a.equalsDottedField("a"));
    ASSERT(!a.equalsDottedField("b"));
    ASSERT(!a.equalsDottedField("a.b.c"));
}

TEST(Equality, Simple2) {
    FieldRef a("a");
    ASSERT(!a.equalsDottedField("a.b"));
    ASSERT(a.equalsDottedField("a"));
    ASSERT(!a.equalsDottedField("b"));
    ASSERT(!a.equalsDottedField("a.b.c"));
}

TEST(Comparison, BothEmpty) {
    FieldRef a;
    ASSERT_TRUE(a == a);
    ASSERT_FALSE(a != a);
    ASSERT_FALSE(a < a);
    ASSERT_TRUE(a <= a);
    ASSERT_FALSE(a > a);
    ASSERT_TRUE(a >= a);
}

TEST(Comparison, EqualInSize) {
    FieldRef a("a.b.c"), b("a.d.c");
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(a < b);
    ASSERT_TRUE(a <= b);
    ASSERT_FALSE(a > b);
    ASSERT_FALSE(a >= b);
}

TEST(Comparison, NonEqual) {
    FieldRef a("a.b.c"), b("b.d");
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(a < b);
    ASSERT_TRUE(a <= b);
    ASSERT_FALSE(a > b);
    ASSERT_FALSE(a >= b);
}

TEST(Comparison, MixedEmtpyAndNot) {
    FieldRef a("a"), b;
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_FALSE(a < b);
    ASSERT_FALSE(a <= b);
    ASSERT_TRUE(a > b);
    ASSERT_TRUE(a >= b);
}

TEST(DottedField, Simple1) {
    FieldRef a("a.b.c.d.e");
    ASSERT_EQUALS("a.b.c.d.e", a.dottedField());
    ASSERT_EQUALS("a.b.c.d.e", a.dottedField(0));
    ASSERT_EQUALS("b.c.d.e", a.dottedField(1));
    ASSERT_EQUALS("c.d.e", a.dottedField(2));
    ASSERT_EQUALS("d.e", a.dottedField(3));
    ASSERT_EQUALS("e", a.dottedField(4));
    ASSERT_EQUALS("", a.dottedField(5));
    ASSERT_EQUALS("", a.dottedField(6));
}

TEST(DottedSubstring, Short) {
    FieldRef path("a");
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("a", path.dottedSubstring(0, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(1, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(0, 0));
}

TEST(DottedSubstring, Empty) {
    FieldRef path("");
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedSubstring(0, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(1, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(0, 0));
}

TEST(DottedSubstring, Nested) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    ASSERT_EQUALS("b.c.d.e", path.dottedSubstring(1, path.numParts()));
    ASSERT_EQUALS("c.d.e", path.dottedSubstring(2, path.numParts()));
    ASSERT_EQUALS("d.e", path.dottedSubstring(3, path.numParts()));
    ASSERT_EQUALS("e", path.dottedSubstring(4, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(5, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(6, path.numParts()));

    ASSERT_EQUALS("a.b.c.d.e", path.dottedSubstring(0, path.numParts()));
    ASSERT_EQUALS("a.b.c.d", path.dottedSubstring(0, path.numParts() - 1));
    ASSERT_EQUALS("a.b.c", path.dottedSubstring(0, path.numParts() - 2));
    ASSERT_EQUALS("a.b", path.dottedSubstring(0, path.numParts() - 3));
    ASSERT_EQUALS("a", path.dottedSubstring(0, path.numParts() - 4));
    ASSERT_EQUALS("", path.dottedSubstring(0, path.numParts() - 5));
    ASSERT_EQUALS("", path.dottedSubstring(0, path.numParts() - 6));

    ASSERT_EQUALS("b.c.d", path.dottedSubstring(1, path.numParts() - 1));
    ASSERT_EQUALS("b.c", path.dottedSubstring(1, path.numParts() - 2));
    ASSERT_EQUALS("b", path.dottedSubstring(1, path.numParts() - 3));
    ASSERT_EQUALS("", path.dottedSubstring(1, path.numParts() - 4));
    ASSERT_EQUALS("", path.dottedSubstring(1, path.numParts() - 5));
}

// The "short" append tests operate entirely in "reserve" space.
TEST(AppendShort, Simple) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a.b.c", path.dottedField());
}

TEST(AppendShort, AppendAndReplace1) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(1, "0");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a.0.c", path.dottedField());
}

TEST(AppendShort, AppendAndReplace2) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(2, "0");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a.b.0", path.dottedField());
}

TEST(AppendShort, ReplaceAndAppend) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.setPart(1, "0");
    path.appendPart("c");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("a.0.c", path.dottedField());
}

TEST(AppendShort, AppendAndGetPart) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(1, "0");
    ASSERT_EQUALS("a", path.getPart(0));
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("c", path.getPart(2));
}

TEST(AppendShort, AppendEmptyPart) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("", path.getPart(2));
    ASSERT_EQUALS("a.b.", path.dottedField());
}

TEST(AppendShort, SetEmptyPartThenAppend) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.setPart(1, "");
    path.appendPart("c");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a..c", path.dottedField());
    ASSERT_EQUALS("", path.getPart(1));
}

TEST(FieldRefTest, ContainsTooManyDots) {
    std::string path(255, '.');
    ASSERT_THROWS_CODE(FieldRef(path), AssertionException, 10396001);
}

// The "medium" append tests feature an append operation that spills out of reserve space (i.e.,
// we append to a path that has _size == kReserveAhead).
TEST(AppendMedium, Simple) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e", path.dottedField());
}

TEST(AppendMedium, AppendAndReplace1) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.setPart(1, "0");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.0.c.d.e", path.dottedField());
}

TEST(AppendMedium, AppendAndReplace2) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.setPart(4, "0");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.0", path.dottedField());
}

TEST(AppendMedium, ReplaceAndAppend) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.setPart(1, "0");
    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("a.0.c.d.e", path.dottedField());
}

TEST(AppendMedium, AppendAndGetPart) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.setPart(1, "0");
    ASSERT_EQUALS("a", path.getPart(0));
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("c", path.getPart(2));
    ASSERT_EQUALS("d", path.getPart(3));
    ASSERT_EQUALS("e", path.getPart(4));
}

TEST(AppendMedium, AppendEmptyPart) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("", path.getPart(4));
    ASSERT_EQUALS("a.b.c.d.", path.dottedField());
}

TEST(AppendMedium, SetEmptyPartThenAppend) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.setPart(1, "");
    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a..c.d.e", path.dottedField());
    ASSERT_EQUALS("", path.getPart(1));
}

// The "long" append tests have paths that are bigger than the reserve space throughout their life
// cycle.
TEST(AppendLong, Simple) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    ASSERT_EQUALS(9u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f.g.h.i", path.dottedField());
}

TEST(AppendLong, AppendAndReplace1) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    path.setPart(1, "0");
    path.setPart(5, "1");
    ASSERT_EQUALS(9u, path.numParts());
    ASSERT_EQUALS("a.0.c.d.e.1.g.h.i", path.dottedField());
}

TEST(AppendLong, AppendAndReplace2) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    path.setPart(7, "0");
    ASSERT_EQUALS(9u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f.g.0.i", path.dottedField());
}

TEST(AppendLong, ReplaceAndAppend) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.setPart(1, "0");
    path.setPart(5, "1");
    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    ASSERT_EQUALS(9u, path.numParts());
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("1", path.getPart(5));
    ASSERT_EQUALS("a.0.c.d.e.1.g.h.i", path.dottedField());
}

TEST(AppendLong, AppendAndGetPart) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    path.setPart(1, "0");
    path.setPart(5, "1");
    path.setPart(7, "2");
    ASSERT_EQUALS("a", path.getPart(0));
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("c", path.getPart(2));
    ASSERT_EQUALS("d", path.getPart(3));
    ASSERT_EQUALS("e", path.getPart(4));
    ASSERT_EQUALS("1", path.getPart(5));
    ASSERT_EQUALS("g", path.getPart(6));
    ASSERT_EQUALS("2", path.getPart(7));
    ASSERT_EQUALS("i", path.getPart(8));
}

TEST(AppendLong, AppendEmptyPart) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("");
    ASSERT_EQUALS(7u, path.numParts());
    ASSERT_EQUALS("", path.getPart(6));
    ASSERT_EQUALS("a.b.c.d.e.f.", path.dottedField());
}

TEST(AppendLong, SetEmptyPartThenAppend) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.setPart(1, "");
    path.setPart(4, "");
    path.appendPart("g");
    ASSERT_EQUALS(7u, path.numParts());
    ASSERT_EQUALS("a..c.d..f.g", path.dottedField());
    ASSERT_EQUALS("", path.getPart(1));
    ASSERT_EQUALS("", path.getPart(4));
}

// The "short" removeLastPart tests operate entirely in "reserve" space.
TEST(RemoveLastPartShort, Simple) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("a", path.dottedField());
}

TEST(RemoveLastPartShort, RemoveUntilEmpty) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    path.removeLastPart();
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedField());
}

TEST(RemoveLastPartShort, AppendThenRemove) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.removeLastPart();
    ASSERT_EQUALS(2u, path.numParts());
    ASSERT_EQUALS("a.b", path.dottedField());
}

TEST(RemoveLastPartShort, RemoveThenAppend) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    path.appendPart("b");
    ASSERT_EQUALS(2u, path.numParts());
    ASSERT_EQUALS("a.b", path.dottedField());
}

TEST(RemoveLastPartShort, RemoveThenSetPart) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    path.setPart(0, "0");
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("0", path.dottedField());
}

TEST(RemoveLastPartShort, SetPartThenRemove) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.setPart(0, "0");
    path.setPart(1, "1");
    path.removeLastPart();
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("0", path.dottedField());
}

TEST(RemoveLastPartShort, AppendThenSetPartThenRemove) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(2, "0");
    path.removeLastPart();
    ASSERT_EQUALS(2u, path.numParts());
    ASSERT_EQUALS("a.b", path.dottedField());
}

// The "medium" removeLastPart tests feature paths that change in size during the test so that they
// transition between fitting and not fitting in "reserve" space.
TEST(RemoveLastPartMedium, Simple) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("a.b.c.d", path.dottedField());
}

TEST(RemoveLastPartMedium, RemoveUntilEmpty) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    while (path.numParts() > 0) {
        path.removeLastPart();
    }
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedField());
}

TEST(RemoveLastPartMedium, AppendThenRemove) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("a.b.c.d", path.dottedField());
}

TEST(RemoveLastPartMedium, RemoveThenAppend) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.removeLastPart();
    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e", path.dottedField());
}

TEST(RemoveLastPartMedium, RemoveThenSetPart) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.removeLastPart();
    path.setPart(0, "0");
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("0.b.c.d", path.dottedField());
}

TEST(RemoveLastPartMedium, SetPartThenRemove) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.setPart(0, "0");
    path.setPart(4, "1");
    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("0.b.c.d", path.dottedField());
}

TEST(RemoveLastPartMedium, AppendThenSetPartThenRemove) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("c");
    path.setPart(2, "0");
    path.setPart(4, "1");
    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("a.b.0.d", path.dottedField());
}

// The "long" removeLastPart tests have paths that are bigger than the reserve space throughout
// their life cycle (with the exception of RemoveUntilempty).
TEST(RemoveLastPartLong, Simple) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f", path.dottedField());
}

TEST(RemoveLastPartLong, RemoveUntilEmpty) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    while (path.numParts() > 0) {
        path.removeLastPart();
    }
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedField());
}

TEST(RemoveLastPartLong, AppendThenRemove) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f", path.dottedField());
}

TEST(RemoveLastPartLong, RemoveThenAppend) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.removeLastPart();
    path.appendPart("g");
    ASSERT_EQUALS(7u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f.g", path.dottedField());
}

TEST(RemoveLastPartLong, RemoveThenSetPart) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.removeLastPart();
    path.setPart(0, "0");
    path.setPart(4, "1");
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("0.b.c.d.1.f", path.dottedField());
}

TEST(RemoveLastPartLong, SetPartThenRemove) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.setPart(0, "0");
    path.setPart(4, "1");
    path.setPart(6, "2");
    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("0.b.c.d.1.f", path.dottedField());
}

TEST(RemoveLastPartLong, AppendThenSetPartThenRemove) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.setPart(2, "0");
    path.setPart(4, "1");
    path.setPart(6, "2");
    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("a.b.0.d.1.f", path.dottedField());
}

TEST(RemoveLastPartLong, FieldRefCopyConstructor) {
    FieldRef original("a.b.c");
    FieldRef copy = original;
    ASSERT_EQ(original, copy);
}

TEST(RemoveLastPartLong, FieldRefCopyAssignment) {
    FieldRef original("a.b.c");
    FieldRef other("x.y.z");
    ASSERT_NE(original, other);
    other = original;
    ASSERT_EQ(original, other);
}

TEST(RemoveLastPartLong, FieldRefFromCopyAssignmentIsValidAfterOriginalIsDeleted) {
    FieldRef copy("x.y.z");
    {
        FieldRef original("a.b.c");
        ASSERT_NE(original, copy);
        copy = original;
    }
    ASSERT_EQ(copy, FieldRef("a.b.c"));
}

TEST(RemoveLastPartLong, FieldRefFromCopyAssignmentIsADeepCopy) {
    FieldRef original("a.b.c");
    FieldRef other("x.y.z");
    ASSERT_NE(original, other);
    other = original;
    ASSERT_EQ(original, other);

    original.setPart(1u, "foo");
    original.appendPart("bar");
    ASSERT_EQ(original, FieldRef("a.foo.c.bar"));

    ASSERT_EQ(other, FieldRef("a.b.c"));
}

TEST(NumericPathComponents, CanIdentifyNumericPathComponents) {
    FieldRef path("a.0.b.1.c");
    ASSERT(!path.isNumericPathComponentStrict(0));
    ASSERT(path.isNumericPathComponentStrict(1));
    ASSERT(!path.isNumericPathComponentStrict(2));
    ASSERT(path.isNumericPathComponentStrict(3));
    ASSERT(!path.isNumericPathComponentStrict(4));
}

TEST(NumericPathComponents, CanObtainAllNumericPathComponents) {
    FieldRef path("a.0.b.1.c.2.d");
    std::set<FieldIndex> expectedComponents{FieldIndex(1), FieldIndex(3), FieldIndex(5)};
    auto numericPathComponents = path.getNumericPathComponents();
    ASSERT(numericPathComponents == expectedComponents);
}

TEST(NumericPathComponents, FieldsWithLeadingZeroesAreNotConsideredNumeric) {
    FieldRef path("a.0.b.01.c.2.d");
    std::set<FieldIndex> expectedComponents{FieldIndex(1), FieldIndex(5)};
    auto numericPathComponents = path.getNumericPathComponents();
    ASSERT(numericPathComponents == expectedComponents);
}

TEST(RemoveFirstPart, EmptyPathDoesNothing) {
    FieldRef path;
    path.removeFirstPart();
    ASSERT_EQ(path.numParts(), 0U);
}

TEST(RemoveFirstPart, PathWithOneComponentBecomesEmpty) {
    FieldRef path("first");
    path.removeFirstPart();
    ASSERT_EQ(path.numParts(), 0U);
}

TEST(RemoveFirstPart, PathWithTwoComponentsOnlyHoldsSecond) {
    FieldRef path("remove.keep");
    path.removeFirstPart();
    ASSERT_EQ(path.numParts(), 1U);
    ASSERT_EQ(path, FieldRef("keep"));
}

TEST(RemoveFirstPart, RemovingFirstPartFromLongPathMultipleTimes) {
    FieldRef path("first.second.third.fourth.fifth.sixth.seventh.eigth.ninth.tenth");
    path.removeFirstPart();
    ASSERT_EQ(path, FieldRef("second.third.fourth.fifth.sixth.seventh.eigth.ninth.tenth"));
    path.removeFirstPart();
    ASSERT_EQ(path, FieldRef("third.fourth.fifth.sixth.seventh.eigth.ninth.tenth"));
    path.removeFirstPart();
    ASSERT_EQ(path, FieldRef("fourth.fifth.sixth.seventh.eigth.ninth.tenth"));
    path.removeFirstPart();
    ASSERT_EQ(path, FieldRef("fifth.sixth.seventh.eigth.ninth.tenth"));
    path.removeFirstPart();
    ASSERT_EQ(path, FieldRef("sixth.seventh.eigth.ninth.tenth"));
}

}  // namespace
}  // namespace mongo
