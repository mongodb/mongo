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

#include "mongo/db/field_ref.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string>

namespace mongo {
namespace {

TEST(FieldRefTest, NoFields) {
    FieldRef fieldRef("");
    ASSERT_EQUALS(fieldRef.numParts(), 0U);
    ASSERT_EQUALS(fieldRef.dottedField(), "");
}

TEST(FieldRefTest, NoFieldNames) {
    std::string field = ".";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 2U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(FieldRefTest, NoFieldNames2) {
    std::string field = "..";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 3U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "");
    ASSERT_EQUALS(fieldRef.getPart(2), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(FieldRefTest, EmptyFieldName) {
    std::string field = ".b.";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 3U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "b");
    ASSERT_EQUALS(fieldRef.getPart(2), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(FieldRefTest, ReinitializeWithEmptyString) {
    FieldRef fieldRef("a.b.c.d.e");
    ASSERT_EQUALS(fieldRef.numParts(), 5U);

    fieldRef.parse("");
    ASSERT_EQUALS(fieldRef.numParts(), 0U);
    ASSERT_EQUALS(fieldRef.dottedField(), "");
}

TEST(FieldRefTest, SinglePart) {
    std::string field = "a";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), field);
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

DEATH_TEST_REGEX(FieldRefTest, Overflow, "Tripwire assertion.*1589700") {
    std::string field = "a";
    for (size_t s = 1; s <= BSONObjMaxInternalSize / 2; s++) {
        field.append(".a");
    }
    ASSERT_GT(field.size(), BSONObjMaxInternalSize);
    FieldRef fieldRef;
    ASSERT_THROWS_CODE(fieldRef.parse(field), AssertionException, 1589700);
}


TEST(FieldRefTest, ParseTwice) {
    std::string field = "a";
    FieldRef fieldRef;
    for (int i = 0; i < 2; i++) {
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 1U);
        ASSERT_EQUALS(fieldRef.getPart(0), field);
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }
}

TEST(FieldRefTest, MultiplePartsVariable) {
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

TEST(FieldRefTest, ConstructorRejectsEmbeddedNullChars) {
    const auto embeddedNull = "a\0"_sd;
    FieldRef fieldRef;
    ASSERT_THROWS_CODE(FieldRef(embeddedNull), AssertionException, 9867600);
}

TEST(FieldRefTest, ParseRejectsEmbeddedNullChars) {
    const auto embeddedNull = "a\0"_sd;
    FieldRef fieldRef;
    ASSERT_THROWS_CODE(fieldRef.parse(embeddedNull), AssertionException, 9867600);
}

TEST(FieldRefTest, ReplaceSingleField) {
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

TEST(FieldRefTest, ReplaceInMultipleField) {
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

TEST(FieldRefTest, SameFieldMultipleReplacements) {
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

TEST(FieldRefTest, SetPartRejectsEmbeddedNullChars) {
    FieldRef fieldRef("f");
    const auto embeddedNull = "a\0"_sd;
    ASSERT_THROWS_CODE(fieldRef.setPart(0, embeddedNull), AssertionException, 9867600);
}

TEST(FieldRefTest, NormalPrefix) {
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

TEST(FieldRefTest, DottedPrefix) {
    FieldRef prefix("a.0"), base("a.0.c");
    ASSERT_TRUE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));

    prefix.parse("a.0.c");
    ASSERT_FALSE(prefix.isPrefixOf(base));
    ASSERT_TRUE(prefix.isPrefixOfOrEqualTo(base));
}

TEST(FieldRefTest, NoPrefixes) {
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

TEST(FieldRefTest, EmptyPrefix) {
    FieldRef field("a"), empty;
    ASSERT_FALSE(field.isPrefixOf(empty));
    ASSERT_FALSE(empty.isPrefixOf(field));
    ASSERT_FALSE(empty.isPrefixOf(empty));
}

TEST(FieldRefTest, CommonPrefixSizeNormal) {
    FieldRef fieldA("a.b"), fieldB("a");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 1U);

    fieldB.parse("a.b");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 2U);

    fieldB.parse("a.b.c");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 2U);
}

TEST(FieldRefTest, CommonPrefixNoCommonality) {
    FieldRef fieldA, fieldB;
    fieldA.parse("a");
    fieldB.parse("b");
    ASSERT_EQUALS(fieldA.commonPrefixSize(fieldB), 0U);
}

TEST(FieldRefTest, CommonPrefixSizeEmpty) {
    FieldRef fieldA("a"), empty;
    ASSERT_EQUALS(fieldA.commonPrefixSize(empty), 0U);
    ASSERT_EQUALS(empty.commonPrefixSize(fieldA), 0U);
}

TEST(FieldRefTest, FullyOverlapsWithNormal) {
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

TEST(FieldRefTest, FullyOverlapsWithNoCommonality) {
    FieldRef fieldA("a.b.c"), fieldB("b.c.d");
    ASSERT_FALSE(fieldA.fullyOverlapsWith(fieldB));
    ASSERT_FALSE(fieldB.fullyOverlapsWith(fieldA));
}

TEST(FieldRefTest, FullyOverlapsWithEmpty) {
    FieldRef field("a"), empty;
    ASSERT_FALSE(field.fullyOverlapsWith(empty));
    ASSERT_FALSE(empty.fullyOverlapsWith(field));
}

TEST(FieldRefTest, EqualitySimple1) {
    FieldRef a("a.b");
    ASSERT(a.equalsDottedField("a.b"));
    ASSERT(!a.equalsDottedField("a"));
    ASSERT(!a.equalsDottedField("b"));
    ASSERT(!a.equalsDottedField("a.b.c"));
}

TEST(FieldRefTest, EqualitySimple2) {
    FieldRef a("a");
    ASSERT(!a.equalsDottedField("a.b"));
    ASSERT(a.equalsDottedField("a"));
    ASSERT(!a.equalsDottedField("b"));
    ASSERT(!a.equalsDottedField("a.b.c"));
}

TEST(FieldRefTest, ComparisonBothEmpty) {
    FieldRef a;
    ASSERT_TRUE(a == a);
    ASSERT_FALSE(a != a);
    ASSERT_FALSE(a < a);
    ASSERT_TRUE(a <= a);
    ASSERT_FALSE(a > a);
    ASSERT_TRUE(a >= a);
}

TEST(FieldRefTest, ComparisonEqualInSize) {
    FieldRef a("a.b.c"), b("a.d.c");
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(a < b);
    ASSERT_TRUE(a <= b);
    ASSERT_FALSE(a > b);
    ASSERT_FALSE(a >= b);
}

TEST(FieldRefTest, ComparisonNonEqual) {
    FieldRef a("a.b.c"), b("b.d");
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(a < b);
    ASSERT_TRUE(a <= b);
    ASSERT_FALSE(a > b);
    ASSERT_FALSE(a >= b);
}

TEST(FieldRefTest, ComparisonMixedEmptyAndNot) {
    FieldRef a("a"), b;
    ASSERT_FALSE(a == b);
    ASSERT_TRUE(a != b);
    ASSERT_FALSE(a < b);
    ASSERT_FALSE(a <= b);
    ASSERT_TRUE(a > b);
    ASSERT_TRUE(a >= b);
}

TEST(FieldRefTest, DottedFieldSimple1) {
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

TEST(FieldRefTest, DottedSubstringShort) {
    FieldRef path("a");
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("a", path.dottedSubstring(0, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(1, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(0, 0));
}

TEST(FieldRefTest, DottedSubstringEmpty) {
    FieldRef path("");
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedSubstring(0, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(1, path.numParts()));
    ASSERT_EQUALS("", path.dottedSubstring(0, 0));
}

TEST(FieldRefTest, DottedSubstringNested) {
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
TEST(FieldRefTest, AppendShortSimple) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a.b.c", path.dottedField());
}

TEST(FieldRefTest, AppendAndReplaceShort1) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(1, "0");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a.0.c", path.dottedField());
}

TEST(FieldRefTest, AppendAndReplaceShort2) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(2, "0");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a.b.0", path.dottedField());
}

TEST(FieldRefTest, ReplaceAndAppendShort) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.setPart(1, "0");
    path.appendPart("c");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("a.0.c", path.dottedField());
}

TEST(FieldRefTest, AppendShortAndGetPart) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.setPart(1, "0");
    ASSERT_EQUALS("a", path.getPart(0));
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("c", path.getPart(2));
}

TEST(FieldRefTest, AppendEmptyPart) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("", path.getPart(2));
    ASSERT_EQUALS("a.b.", path.dottedField());
}

TEST(FieldRefTest, SetEmptyPartThenAppendShort) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.setPart(1, "");
    path.appendPart("c");
    ASSERT_EQUALS(3u, path.numParts());
    ASSERT_EQUALS("a..c", path.dottedField());
    ASSERT_EQUALS("", path.getPart(1));
}

TEST(FieldRefTest, AppendPartRejectsEmbeddedNullChars) {
    FieldRef fieldRef("f");
    const auto embeddedNull = "a\0"_sd;
    ASSERT_THROWS_CODE(fieldRef.appendPart(embeddedNull), AssertionException, 9867600);
    ASSERT_THROWS_CODE(
        FieldRef::FieldRefTempAppend(fieldRef, embeddedNull), AssertionException, 9867600);
}

TEST(FieldRefTest, ContainsTooManyDots) {
    std::string path(255, '.');
    ASSERT_THROWS_CODE(FieldRef(path), AssertionException, 10396001);
}

// The "medium" append tests feature an append operation that spills out of reserve space (i.e.,
// we append to a path that has _size == kReserveAhead).
TEST(FieldRefTest, AppendMediumSimple) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e", path.dottedField());
}

TEST(FieldRefTest, AppendAndReplaceMedium1) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.setPart(1, "0");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.0.c.d.e", path.dottedField());
}

TEST(FieldRefTest, AppendAndReplaceMedium2) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.setPart(4, "0");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.0", path.dottedField());
}

TEST(FieldRefTest, ReplaceAndAppendMedium) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.setPart(1, "0");
    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("0", path.getPart(1));
    ASSERT_EQUALS("a.0.c.d.e", path.dottedField());
}

TEST(FieldRefTest, AppendAndGetPartMedium) {
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

TEST(FieldRefTest, AppendEmptyPartToMedium) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("", path.getPart(4));
    ASSERT_EQUALS("a.b.c.d.", path.dottedField());
}

TEST(FieldRefTest, SetEmptyPartThenAppendToMedium) {
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
TEST(FieldRefTest, AppendLongSimple) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    ASSERT_EQUALS(9u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f.g.h.i", path.dottedField());
}

TEST(FieldRefTest, AppendAndReplaceLong1) {
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

TEST(FieldRefTest, AppendAndReplaceLong2) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.appendPart("h");
    path.appendPart("i");
    path.setPart(7, "0");
    ASSERT_EQUALS(9u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f.g.0.i", path.dottedField());
}

TEST(FieldRefTest, ReplaceAndAppendLong) {
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

TEST(FieldRefTest, AppendAndGetPartLong) {
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

TEST(FieldRefTest, AppendEmptyPartToLong) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("");
    ASSERT_EQUALS(7u, path.numParts());
    ASSERT_EQUALS("", path.getPart(6));
    ASSERT_EQUALS("a.b.c.d.e.f.", path.dottedField());
}

TEST(FieldRefTest, SetEmptyPartThenAppendToLong) {
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
TEST(FieldRefTest, RemoveLastParthShortSimple) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("a", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartShortUntilEmpty) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    path.removeLastPart();
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedField());
}

TEST(FieldRefTest, AppendThenRemoveLastPartShort) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.appendPart("c");
    path.removeLastPart();
    ASSERT_EQUALS(2u, path.numParts());
    ASSERT_EQUALS("a.b", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartShortThenAppend) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    path.appendPart("b");
    ASSERT_EQUALS(2u, path.numParts());
    ASSERT_EQUALS("a.b", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartShortThenSetPart) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.removeLastPart();
    path.setPart(0, "0");
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("0", path.dottedField());
}

TEST(FieldRefTest, SetPartThenRemoveLastPartShort) {
    FieldRef path("a.b");
    ASSERT_EQUALS(2u, path.numParts());

    path.setPart(0, "0");
    path.setPart(1, "1");
    path.removeLastPart();
    ASSERT_EQUALS(1u, path.numParts());
    ASSERT_EQUALS("0", path.dottedField());
}

TEST(FieldRefTest, AppendThenSetPartThenRemoveLastPartShort) {
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
TEST(FieldRefTest, RemoveLastPartMediumSimple) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("a.b.c.d", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartMediumUntilEmpty) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    while (path.numParts() > 0) {
        path.removeLastPart();
    }
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedField());
}

TEST(FieldRefTest, AppendThenRemoveLastPartMedium) {
    FieldRef path("a.b.c.d");
    ASSERT_EQUALS(4u, path.numParts());

    path.appendPart("e");
    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("a.b.c.d", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartMediumThenAppend) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.removeLastPart();
    path.appendPart("e");
    ASSERT_EQUALS(5u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartMediumThenSetPart) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.removeLastPart();
    path.setPart(0, "0");
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("0.b.c.d", path.dottedField());
}

TEST(FieldRefTest, SetPartThenRemoveLastPartMedium) {
    FieldRef path("a.b.c.d.e");
    ASSERT_EQUALS(5u, path.numParts());

    path.setPart(0, "0");
    path.setPart(4, "1");
    path.removeLastPart();
    ASSERT_EQUALS(4u, path.numParts());
    ASSERT_EQUALS("0.b.c.d", path.dottedField());
}

TEST(FieldRefTest, AppendThenSetPartThenRemoveLastPartMedium) {
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
TEST(FieldRefTest, RemoveLastPartLongSimple) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartLongUntilEmpty) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    while (path.numParts() > 0) {
        path.removeLastPart();
    }
    ASSERT_EQUALS(0u, path.numParts());
    ASSERT_EQUALS("", path.dottedField());
}

TEST(FieldRefTest, AppendThenRemoveLastPartLong) {
    FieldRef path("a.b.c.d.e.f");
    ASSERT_EQUALS(6u, path.numParts());

    path.appendPart("g");
    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartLongThenAppend) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.removeLastPart();
    path.appendPart("g");
    ASSERT_EQUALS(7u, path.numParts());
    ASSERT_EQUALS("a.b.c.d.e.f.g", path.dottedField());
}

TEST(FieldRefTest, RemoveLastPartLongThenSetPart) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.removeLastPart();
    path.setPart(0, "0");
    path.setPart(4, "1");
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("0.b.c.d.1.f", path.dottedField());
}

TEST(FieldRefTest, SetPartThenRemoveLastPartLong) {
    FieldRef path("a.b.c.d.e.f.g");
    ASSERT_EQUALS(7u, path.numParts());

    path.setPart(0, "0");
    path.setPart(4, "1");
    path.setPart(6, "2");
    path.removeLastPart();
    ASSERT_EQUALS(6u, path.numParts());
    ASSERT_EQUALS("0.b.c.d.1.f", path.dottedField());
}

TEST(FieldRefTest, AppendThenSetPartThenRemoveLastPartLong) {
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

TEST(FieldRefTest, CopyConstructor) {
    FieldRef original("a.b.c");
    FieldRef copy = original;
    ASSERT_EQ(original, copy);
}

TEST(FieldRefTest, CopyAssignment) {
    FieldRef original("a.b.c");
    FieldRef other("x.y.z");
    ASSERT_NE(original, other);
    other = original;
    ASSERT_EQ(original, other);
}

TEST(FieldRefTest, FromCopyAssignmentIsValidAfterOriginalIsDeleted) {
    FieldRef copy("x.y.z");
    {
        FieldRef original("a.b.c");
        ASSERT_NE(original, copy);
        copy = original;
    }
    ASSERT_EQ(copy, FieldRef("a.b.c"));
}

TEST(FieldRefTest, FromCopyAssignmentIsADeepCopy) {
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

TEST(FieldRefTest, CanIdentifyNumericPathComponents) {
    FieldRef path("a.0.b.1.c");
    ASSERT(!path.isNumericPathComponentStrict(0));
    ASSERT(path.isNumericPathComponentStrict(1));
    ASSERT(!path.isNumericPathComponentStrict(2));
    ASSERT(path.isNumericPathComponentStrict(3));
    ASSERT(!path.isNumericPathComponentStrict(4));
}

TEST(FieldRefTest, CanObtainAllNumericPathComponents) {
    FieldRef path("a.0.b.1.c.2.d");
    std::set<FieldIndex> expectedComponents{FieldIndex(1), FieldIndex(3), FieldIndex(5)};
    auto numericPathComponents = path.getNumericPathComponents();
    ASSERT(numericPathComponents == expectedComponents);
}

TEST(FieldRefTest, FieldsWithLeadingZeroesAreNotConsideredNumeric) {
    FieldRef path("a.0.b.01.c.2.d");
    std::set<FieldIndex> expectedComponents{FieldIndex(1), FieldIndex(5)};
    auto numericPathComponents = path.getNumericPathComponents();
    ASSERT(numericPathComponents == expectedComponents);
}

TEST(FieldRefTest, RemoveFirstPartOnEmptyPathDoesNothing) {
    FieldRef path;
    path.removeFirstPart();
    ASSERT_EQ(path.numParts(), 0U);
}

TEST(FieldRefTest, RemoveFirstPartPathWithOneComponentBecomesEmpty) {
    FieldRef path("first");
    path.removeFirstPart();
    ASSERT_EQ(path.numParts(), 0U);
}

TEST(FieldRefTest, RemoveFirstPartPathWithTwoComponentsOnlyHoldsSecond) {
    FieldRef path("remove.keep");
    path.removeFirstPart();
    ASSERT_EQ(path.numParts(), 1U);
    ASSERT_EQ(path, FieldRef("keep"));
}

TEST(FieldRefTest, RemovingFirstPartFromLongPathMultipleTimes) {
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

TEST(FieldRefTest, CanonicalIndexField) {
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a")), FieldRef("a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("aaa")), FieldRef("aaa"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.b")), FieldRef("a.b"_sd));

    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.$")), FieldRef("a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.0")), FieldRef("a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.123")), FieldRef("a"_sd));

    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.$.b")), FieldRef("a.b"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.0.b")), FieldRef("a.b"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.123.b")), FieldRef("a.b"_sd));

    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.$ref")), FieldRef("a.$ref"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.$ref.b")), FieldRef("a.$ref.b"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.c$d.b")), FieldRef("a.c$d.b"_sd));

    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.123a")), FieldRef("a.123a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.a123")), FieldRef("a.a123"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.123a.b")), FieldRef("a.123a.b"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.a123.b")), FieldRef("a.a123.b"_sd));

    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.")), FieldRef("a."_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("$")), FieldRef("$"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("$.a")), FieldRef("$.a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.$")), FieldRef("a"_sd));
}

TEST(FieldRefTest, CanonicalIndexFieldForNestedNumericFieldNames) {
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.0.0")), FieldRef("a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.55.01")), FieldRef("a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.0.0.b.1")), FieldRef("a"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.0b.1")), FieldRef("a.0b"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.0.b.1.2")), FieldRef("a.b"_sd));
    ASSERT_EQ(FieldRef::getCanonicalIndexField(FieldRef("a.01.02.b.c")), FieldRef("a"_sd));
}

}  // namespace
}  // namespace mongo
