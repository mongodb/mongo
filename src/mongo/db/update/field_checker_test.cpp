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

#include "mongo/db/update/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"

#include <memory>

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
