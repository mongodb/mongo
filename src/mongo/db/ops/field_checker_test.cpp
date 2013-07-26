/**
 *    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
    using mongo::fieldchecker::isUpdatableLegacy;
    using mongo::fieldchecker::isPositional;
    using mongo::Status;

    TEST(IsUpdatable, Basics) {
        FieldRef fieldRef;
        fieldRef.parse("x");
        ASSERT_OK(isUpdatable(fieldRef));
    }

    TEST(IsUpdatable, DottedFields) {
        FieldRef fieldRef;
        fieldRef.parse("x.y.z");
        ASSERT_OK(isUpdatable(fieldRef));
    }

    TEST(IsUpdatable, EmptyFields) {
        FieldRef fieldRef;
        fieldRef.parse("");
        ASSERT_NOT_OK(isUpdatable(fieldRef));

        FieldRef fieldRefDot;
        fieldRefDot.parse(".");
        ASSERT_NOT_OK(isUpdatable(fieldRefDot));

        FieldRef fieldRefDollar;
        fieldRefDollar.parse("$");
        ASSERT_NOT_OK(isUpdatable(fieldRefDollar));

        FieldRef fieldRefADot;
        fieldRefADot.parse("a.");
        ASSERT_NOT_OK(isUpdatable(fieldRefADot));

        FieldRef fieldRefDotB;
        fieldRefDotB.parse(".b");
        ASSERT_NOT_OK(isUpdatable(fieldRefDotB));

        FieldRef fieldRefEmptyMiddle;
        fieldRefEmptyMiddle.parse("a..b");
        ASSERT_NOT_OK(isUpdatable(fieldRefEmptyMiddle));
    }

    TEST(IsUpdatable, SpecialIDField) {
        FieldRef fieldRefID;
        fieldRefID.parse("_id");
        ASSERT_NOT_OK(isUpdatable(fieldRefID));

        FieldRef fieldRefIDX;
        fieldRefIDX.parse("_id.x");
        ASSERT_NOT_OK(isUpdatable(fieldRefIDX));

        FieldRef fieldRefXID;
        fieldRefXID.parse("x._id");
        ASSERT_OK(isUpdatable(fieldRefXID));

        FieldRef fieldRefXIDZ;
        fieldRefXIDZ.parse("x._id.z");
        ASSERT_OK(isUpdatable(fieldRefXIDZ));
    }

    TEST(IsUpdatable, PositionalFields) {
        FieldRef fieldRefXDollar;
        fieldRefXDollar.parse("x.$");
        ASSERT_OK(isUpdatable(fieldRefXDollar));

        FieldRef fieldRefXDollarZ;
        fieldRefXDollarZ.parse("x.$.z");
        ASSERT_OK(isUpdatable(fieldRefXDollarZ));

        // A document never starts with an array.
        FieldRef fieldRefDollarB;
        fieldRefDollarB.parse("$.b");
        ASSERT_NOT_OK(isUpdatable(fieldRefDollarB));

        FieldRef fieldRefDollar;
        fieldRefDollar.parse("$foo");
        ASSERT_NOT_OK(isUpdatable(fieldRefDollar));
    }

    TEST(IsUpdatable, DollarPrefixedDeepFields) {
        FieldRef fieldRefLateDollar;
        fieldRefLateDollar.parse("a.$b");
        ASSERT_NOT_OK(isUpdatable(fieldRefLateDollar));
    }

    TEST(IsUpdatable, DBRefNamesAreIgnored) {
        FieldRef fieldRefDBRef;
        fieldRefDBRef.parse("$id.$ref.$db");
        ASSERT_OK(isUpdatable(fieldRefDBRef));
    }

    TEST(IsUpdatableLegacy, Basics) {
        FieldRef fieldRefDollar;
        fieldRefDollar.parse("$foo");
        ASSERT_OK(isUpdatableLegacy(fieldRefDollar));
    }

    TEST(IsUpdatableLegacy, DollarPrefixedDeepFields) {
        FieldRef fieldRefLateDollar;
        fieldRefLateDollar.parse("a.$b");
        ASSERT_OK(isUpdatableLegacy(fieldRefLateDollar));
    }

    TEST(isPositional, EntireArrayItem) {
        FieldRef fieldRefPositional;
        fieldRefPositional.parse("a.$");
        size_t pos;
        size_t count;
        ASSERT_TRUE(isPositional(fieldRefPositional, &pos, &count));
        ASSERT_EQUALS(pos, 1u);
        ASSERT_EQUALS(count, 1u);
    }

    TEST(isPositional, ArraySubObject) {
        FieldRef fieldRefPositional;
        fieldRefPositional.parse("a.$.b");
        size_t pos;
        size_t count;
        ASSERT_TRUE(isPositional(fieldRefPositional, &pos, &count));
        ASSERT_EQUALS(pos, 1u);
        ASSERT_EQUALS(count, 1u);
    }

    TEST(isPositional, MultiplePositional) {
        FieldRef fieldRefPositional;
        fieldRefPositional.parse("a.$.b.$.c");
        size_t pos;
        size_t count;
        ASSERT_TRUE(isPositional(fieldRefPositional, &pos, &count));
        ASSERT_EQUALS(pos, 1u);
        ASSERT_EQUALS(count, 2u);
    }

} // unnamed namespace
