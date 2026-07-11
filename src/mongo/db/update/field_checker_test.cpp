// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"


namespace {

using mongo::FieldRef;
using mongo::Status;
using mongo::fieldchecker::isArrayFilterIdentifier;
using mongo::fieldchecker::isPositional;
using mongo::fieldchecker::isUpdatable;

TEST(IsUpdatable, Basics) {
    FieldRef fieldRef("x");
    ASSERT_OK(isUpdatable(fieldRef));
}

TEST(IsUpdatable, DottedFields) {
    FieldRef fieldRef("x.y.z");
    ASSERT_OK(isUpdatable(fieldRef));
}

TEST(IsUpdatable, EmptyFields) {
    FieldRef fieldRef("");
    ASSERT_NOT_OK(isUpdatable(fieldRef));

    FieldRef fieldRefDot(".");
    ASSERT_NOT_OK(isUpdatable(fieldRefDot));

    /* TODO: Re-enable after review
    FieldRef fieldRefDollar;
    fieldRefDollar.parse("$");
    ASSERT_NOT_OK(isUpdatable(fieldRefDollar));

*/

    FieldRef fieldRefADot("a.");
    ASSERT_NOT_OK(isUpdatable(fieldRefADot));

    FieldRef fieldRefDotB(".b");
    ASSERT_NOT_OK(isUpdatable(fieldRefDotB));

    FieldRef fieldRefEmptyMiddle;
    fieldRefEmptyMiddle.parse("a..b");
    ASSERT_NOT_OK(isUpdatable(fieldRefEmptyMiddle));
}

// Positional checks
TEST(isPositional, EntireArrayItem) {
    FieldRef fieldRefPositional("a.$");
    size_t pos;
    size_t count;
    ASSERT_TRUE(isPositional(fieldRefPositional, &pos, &count));
    ASSERT_EQUALS(pos, 1u);
    ASSERT_EQUALS(count, 1u);
}

TEST(isPositional, ArraySubObject) {
    FieldRef fieldRefPositional("a.$.b");
    size_t pos;
    size_t count;
    ASSERT_TRUE(isPositional(fieldRefPositional, &pos, &count));
    ASSERT_EQUALS(pos, 1u);
    ASSERT_EQUALS(count, 1u);
}

TEST(isPositional, MultiplePositional) {
    FieldRef fieldRefPositional("a.$.b.$.c");
    size_t pos;
    size_t count;
    ASSERT_TRUE(isPositional(fieldRefPositional, &pos, &count));
    ASSERT_EQUALS(pos, 1u);
    ASSERT_EQUALS(count, 2u);
}

TEST(isArrayFilterIdentifier, Matching) {
    for (const auto& s :
         {"$[fieldName]",                      // Matches a typical field name inside brackets.
          "$[123]",                            // Matches numeric values inside brackets.
          "$[]",                               // Matches empty brackets.
          "$[abc]",                            // Matches alphabetic characters inside brackets.
          "$[innerField.subField]",            // Matches dot-notation field names inside brackets.
          "$[symbols!@#$%^&*()]",              // Matches a variety of symbols inside brackets.
          "$[longStringWithAlphaNumeric123]",  // Matches a mix of alphanumeric characters.
          "$[    ]",                           // Matches brackets containing only whitespace.
          "$[nested $[inner]]"}) {             // Matches nested brackets content.
        ASSERT_TRUE(isArrayFilterIdentifier(s));
    }
}

TEST(isArrayFilterIdentifier, NonMatching) {
    for (const auto& s : {"[fieldName]",         // Missing the "$" prefix.
                          "fieldName",           // Does not contain brackets.
                          "$[fieldName",         // Missing closing bracket "]".
                          "$fieldName]",         // Missing opening bracket "[".
                          "[fieldName]",         // Brackets exist but missing the "$" prefix.
                          "abc$[xyz]",           // "$[...]" does not match the entire string.
                          "$[x] extra text"}) {  // Contains extra text after closing bracket.
        ASSERT_FALSE(isArrayFilterIdentifier(s));
    }
}

}  // unnamed namespace
