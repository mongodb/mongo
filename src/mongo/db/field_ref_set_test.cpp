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

#include "mongo/platform/basic.h"

#include "mongo/db/field_ref_set.h"

#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(EmptySet, Normal) {
    // insert "b"
    FieldRefSet fieldSet;
    FieldRef bSimple("b");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&bSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "a", OK
    FieldRef aSimple("a");
    ASSERT_TRUE(fieldSet.insert(&aSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "c", OK
    FieldRef cSimple("c");
    ASSERT_TRUE(fieldSet.insert(&cSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
}

TEST(EmptySet, Conflict) {
    // insert "a.b"
    FieldRefSet fieldSet;
    FieldRef aDotB("a.b");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&aDotB, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "a", conflicts with "a.b"
    FieldRef prefix("a");
    ASSERT_FALSE(fieldSet.insert(&prefix, &conflict));
    ASSERT_EQUALS(aDotB, *conflict);

    // insert "a.b.c", conflicts with "a.b"
    FieldRef superSet("a.b.c");
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
    FieldRef bDotC("b.c");
    FieldRef bDotE("b.e");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&bDotC, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
    ASSERT_TRUE(fieldSet.insert(&bDotE, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "a" before, OK
    FieldRef aSimple("a");
    ASSERT_TRUE(fieldSet.insert(&aSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "b.d" in the middle, OK
    FieldRef bDotD("b.d");
    ASSERT_TRUE(fieldSet.insert(&bDotD, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "c" after, OK
    FieldRef cSimple("c");
    ASSERT_TRUE(fieldSet.insert(&cSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
}

TEST(NotEmpty, Conflict) {
    // insert "b.c" and "b.e"
    FieldRefSet fieldSet;
    FieldRef bDotC("b.c");
    FieldRef bDotE("b.e");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&bDotC, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);
    ASSERT_TRUE(fieldSet.insert(&bDotE, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(NULL), conflict);

    // insert "b" before, conflicts "b.c"
    FieldRef bSimple("b");
    ASSERT_FALSE(fieldSet.insert(&bSimple, &conflict));
    ASSERT_EQUALS(bDotC, *conflict);

    // insert: "b.c.d" in the "middle", conflicts "b.c"
    FieldRef bDotCDotD("b.c.d");
    ASSERT_FALSE(fieldSet.insert(&bDotCDotD, &conflict));
    ASSERT_EQUALS(bDotC, *conflict);

    // insert: "b.e.f" at the end, conflicts "b.e"
    FieldRef bDotEDotF("b.e.f");
    ASSERT_FALSE(fieldSet.insert(&bDotEDotF, &conflict));
    ASSERT_EQUALS(bDotE, *conflict);
}

TEST(FieldRefSetWithStorageTest, ManagesFieldRefLifetime) {
    FieldRefSetWithStorage fieldRefSet;
    FieldRef ref("a.b");

    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b}", fieldRefSet.toString());

    // Re-use 'ref', and verify that the set still contains the previous FieldRef.
    ref.parse("b.c"_sd);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b, b.c}", fieldRefSet.toString());
}

TEST(FieldRefSetWithStorageTest, InsertRemovesConflictsByKeepingShortest) {
    FieldRefSetWithStorage fieldRefSet;
    FieldRef ref("a.b");

    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b}", fieldRefSet.toString());

    ref.parse("a"_sd);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a}", fieldRefSet.toString());
}

TEST(FieldRefSetWithStorageTest, InsertRemovesMultipleConflicts) {
    FieldRefSetWithStorage fieldRefSet;
    FieldRef ref("a.b");

    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b}", fieldRefSet.toString());

    ref.parse("a.c"_sd);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b, a.c}", fieldRefSet.toString());

    // Inserting 'a' should remove both conflicts with longer paths.
    ref.parse("a"_sd);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a}", fieldRefSet.toString());
}

}  // namespace
}  // namespace mongo
