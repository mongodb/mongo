/**
 *    Copyright 2013 10gen Inc.
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

#include "mongo/db/ops/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::ErrorCodes;
using mongo::FieldRef;
using mongo::fieldchecker::isUpdatable;
using mongo::fieldchecker::isPositional;
using mongo::Status;

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
}  // unnamed namespace
