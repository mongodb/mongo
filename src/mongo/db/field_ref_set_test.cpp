// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/field_ref_set.h"

#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

TEST(EmptySet, Normal) {
    // insert "b"
    FieldRefSet fieldSet;
    FieldRef bSimple("b");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&bSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

    // insert "a", OK
    FieldRef aSimple("a");
    ASSERT_TRUE(fieldSet.insert(&aSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

    // insert "c", OK
    FieldRef cSimple("c");
    ASSERT_TRUE(fieldSet.insert(&cSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);
}

TEST(EmptySet, Conflict) {
    // insert "a.b"
    FieldRefSet fieldSet;
    FieldRef aDotB("a.b");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&aDotB, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

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
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

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
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);
    ASSERT_TRUE(fieldSet.insert(&bDotE, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

    // insert "a" before, OK
    FieldRef aSimple("a");
    ASSERT_TRUE(fieldSet.insert(&aSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

    // insert "b.d" in the middle, OK
    FieldRef bDotD("b.d");
    ASSERT_TRUE(fieldSet.insert(&bDotD, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

    // insert "c" after, OK
    FieldRef cSimple("c");
    ASSERT_TRUE(fieldSet.insert(&cSimple, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);
}

TEST(NotEmpty, Conflict) {
    // insert "b.c" and "b.e"
    FieldRefSet fieldSet;
    FieldRef bDotC("b.c");
    FieldRef bDotE("b.e");
    const FieldRef* conflict;
    ASSERT_TRUE(fieldSet.insert(&bDotC, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);
    ASSERT_TRUE(fieldSet.insert(&bDotE, &conflict));
    ASSERT_EQUALS(static_cast<const FieldRef*>(nullptr), conflict);

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
    ref.parse("b.c"sv);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b, b.c}", fieldRefSet.toString());
}

TEST(FieldRefSetWithStorageTest, InsertRemovesConflictsByKeepingShortest) {
    FieldRefSetWithStorage fieldRefSet;
    FieldRef ref("a.b");

    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b}", fieldRefSet.toString());

    ref.parse("a"sv);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a}", fieldRefSet.toString());
}

TEST(FieldRefSetWithStorageTest, InsertRemovesMultipleConflicts) {
    FieldRefSetWithStorage fieldRefSet;
    FieldRef ref("a.b");

    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b}", fieldRefSet.toString());

    ref.parse("a.c"sv);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a.b, a.c}", fieldRefSet.toString());

    // Inserting 'a' should remove both conflicts with longer paths.
    ref.parse("a"sv);
    fieldRefSet.keepShortest(ref);
    ASSERT_EQUALS("{a}", fieldRefSet.toString());
}

}  // namespace
}  // namespace mongo
