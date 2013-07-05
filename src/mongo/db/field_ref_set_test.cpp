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

#include "mongo/db/field_ref_set.h"

#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::FieldRef;
    using mongo::FieldRefSet;

    TEST(EmptySet, Normal) {
        // insert "b"
        FieldRefSet fieldSet;
        FieldRef bSimple;
        bSimple.parse("b");
        const FieldRef* conflict;
        ASSERT_TRUE(fieldSet.insert(&bSimple, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "a", OK
        FieldRef aSimple;
        aSimple.parse("a");
        ASSERT_TRUE(fieldSet.insert(&aSimple, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "c", OK
        FieldRef cSimple;
        cSimple.parse("c");
        ASSERT_TRUE(fieldSet.insert(&cSimple, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
    }

    TEST(EmptySet, Conflict) {
        // insert "a.b"
        FieldRefSet fieldSet;
        FieldRef aDotB;
        aDotB.parse("a.b");
        const FieldRef* conflict;
        ASSERT_TRUE(fieldSet.insert(&aDotB, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "a", conflicts with "a.b"
        FieldRef prefix;
        prefix.parse("a");
        ASSERT_FALSE(fieldSet.insert(&prefix, &conflict));
        ASSERT_EQUALS(aDotB, *conflict);

        // insert "a.b.c", conflicts with "a.b"
        FieldRef superSet;
        superSet.parse("a.b.c");
        ASSERT_FALSE(fieldSet.insert(&superSet, &conflict));
        ASSERT_EQUALS(aDotB, *conflict);
    }

    TEST(EmptySet, EmptyField) {
        // Old data may have empty field names. We test that we can catch conflicts if we try
        // to insert an empty field twice.
        FieldRefSet fieldSet;
        FieldRef empty;
        const FieldRef* conflict;
        ASSERT_TRUE(fieldSet.insert(&empty, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        ASSERT_FALSE(fieldSet.insert(&empty, &conflict));
        ASSERT_EQUALS(empty, *conflict);
    }

    TEST(NotEmptySet, Normal) {
        // insert "b.c" and "b.e"
        FieldRefSet fieldSet;
        FieldRef bDotC;
        bDotC.parse("b.c");
        FieldRef bDotE;
        bDotE.parse("b.e");
        const FieldRef* conflict;
        ASSERT_TRUE(fieldSet.insert(&bDotC, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
        ASSERT_TRUE(fieldSet.insert(&bDotE, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "a" before, OK
        FieldRef aSimple;
        aSimple.parse("a");
        ASSERT_TRUE(fieldSet.insert(&aSimple, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "b.d" in the middle, OK
        FieldRef bDotD;
        bDotD.parse("b.d");
        ASSERT_TRUE(fieldSet.insert(&bDotD, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "c" after, OK
        FieldRef cSimple;
        cSimple.parse("c");
        ASSERT_TRUE(fieldSet.insert(&cSimple, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
    }

    TEST(NotEmpty, Conflict) {
       // insert "b.c" and "b.e"
        FieldRefSet fieldSet;
        FieldRef bDotC;
        bDotC.parse("b.c");
        FieldRef bDotE;
        bDotE.parse("b.e");
        const FieldRef* conflict;
        ASSERT_TRUE(fieldSet.insert(&bDotC, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
        ASSERT_TRUE(fieldSet.insert(&bDotE, &conflict));
        ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

        // insert "b" before, conflicts "b.c"
        FieldRef bSimple;
        bSimple.parse("b");
        ASSERT_FALSE(fieldSet.insert(&bSimple, &conflict));
        ASSERT_EQUALS(bDotC, *conflict);

        // insert: "b.c.d" in the "middle", conflicts "b.c"
        FieldRef bDotCDotD;
        bDotCDotD.parse("b.c.d");
        ASSERT_FALSE(fieldSet.insert(&bDotCDotD, &conflict));
        ASSERT_EQUALS(bDotC, *conflict);

        // insert: "b.e.f" at the end, conflicts "b.e"
        FieldRef bDotEDotF;
        bDotEDotF.parse("b.e.f");
        ASSERT_FALSE(fieldSet.insert(&bDotEDotF, &conflict));
        ASSERT_EQUALS(bDotE, *conflict);
    }

} // unnamed namespace
