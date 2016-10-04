/*    Copyright 2012 10gen Inc.
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

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using mongo::FieldRef;
using mongo::StringData;
using mongoutils::str::stream;
using std::string;

TEST(Empty, NoFields) {
    FieldRef fieldRef("");
    ASSERT_EQUALS(fieldRef.numParts(), 0U);
    ASSERT_EQUALS(fieldRef.dottedField(), "");
}

TEST(Empty, NoFieldNames) {
    string field = ".";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 2U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Empty, NoFieldNames2) {
    string field = "..";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 3U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "");
    ASSERT_EQUALS(fieldRef.getPart(2), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Empty, EmptyFieldName) {
    string field = ".b.";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 3U);
    ASSERT_EQUALS(fieldRef.getPart(0), "");
    ASSERT_EQUALS(fieldRef.getPart(1), "b");
    ASSERT_EQUALS(fieldRef.getPart(2), "");
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Normal, SinglePart) {
    string field = "a";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), field);
    ASSERT_EQUALS(fieldRef.dottedField(), field);
}

TEST(Normal, ParseTwice) {
    string field = "a";
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
    string field(parts[0]);
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
    string field = "$";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), "$");

    string newField = "a";
    fieldRef.setPart(0, newField);
    ASSERT_EQUALS(fieldRef.numParts(), 1U);
    ASSERT_EQUALS(fieldRef.getPart(0), newField);
    ASSERT_EQUALS(fieldRef.dottedField(), newField);
}

TEST(Replacement, InMultipleField) {
    string field = "a.b.c.$.e";
    FieldRef fieldRef(field);
    ASSERT_EQUALS(fieldRef.numParts(), 5U);
    ASSERT_EQUALS(fieldRef.getPart(3), "$");

    string newField = "d";
    fieldRef.setPart(3, newField);
    ASSERT_EQUALS(fieldRef.numParts(), 5U);
    ASSERT_EQUALS(fieldRef.getPart(3), newField);
    ASSERT_EQUALS(fieldRef.dottedField(), "a.b.c.d.e");
}

TEST(Replacement, SameFieldMultipleReplacements) {
    string prefix = "a.";
    string field = prefix + "$";
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

    prefix.parse("a");
    ASSERT_TRUE(prefix.isPrefixOf(base));
}

TEST(Prefix, Dotted) {
    FieldRef prefix("a.0"), base("a.0.c");
    ASSERT_TRUE(prefix.isPrefixOf(base));
}

TEST(Prefix, NoPrefixes) {
    FieldRef prefix("a.b"), base("a.b");
    ASSERT_FALSE(prefix.isPrefixOf(base));

    base.parse("a");
    ASSERT_FALSE(prefix.isPrefixOf(base));

    base.parse("b");
    ASSERT_FALSE(prefix.isPrefixOf(base));
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

}  // namespace
