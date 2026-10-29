// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/update/update_oplog_entry_serialization.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::update_oplog_entry {
namespace {

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithVersionField,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a" << 1 << "b" << 2);
    BSONObj o(BSON("$v" << 1 << "$set" << setField));

    extractNewValueForField(o, "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithoutVersionField_NotSupported1,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a" << 1 << "b" << 2);
    BSONObj o(BSON("$set" << setField));

    extractNewValueForField(o, "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithoutVersionField_NotSupported2,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a" << 1 << "b" << 2);
    BSONObj o(BSON("$set" << setField));

    isFieldRemovedByUpdate(o, "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithSetAndUnset_NotSupported1,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a" << 1 << "b" << 2);
    auto unsetField = BSON("c" << true << "d" << true);
    BSONObj o(BSON("$set" << setField << "$unset" << unsetField));

    extractNewValueForField(o, "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithSetAndUnset_NotSupported2,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a" << 1 << "b" << 2);
    auto unsetField = BSON("c" << true << "d" << true);
    BSONObj o(BSON("$set" << setField << "$unset" << unsetField));

    isFieldRemovedByUpdate(o, "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWhichIncludesDottedPath_NotSupported1,
                 "Tripwire assertion.*6448500") {
    // While our function for getting modified fields only supports top-level fields,
    // there should be no problem if the oplog entry contains modifications to
    // dotted paths.

    auto setField = BSON("a.b.c" << 1 << "x" << 2);
    auto unsetField = BSON("d.e.f" << true << "y" << true);
    BSONObj o(BSON("$set" << setField << "$unset" << unsetField));

    extractNewValueForField(o, "x");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWhichIncludesDottedPath_NotSupported2,
                 "Tripwire assertion.*6448500") {
    // While our function for getting modified fields only supports top-level fields,
    // there should be no problem if the oplog entry contains modifications to
    // dotted paths.

    auto setField = BSON("a.b.c" << 1 << "x" << 2);
    auto unsetField = BSON("d.e.f" << true << "y" << true);
    BSONObj o(BSON("$set" << setField << "$unset" << unsetField));

    isFieldRemovedByUpdate(o, "x");
}

TEST(UpdateOplogSerializationTest, MakeReplacementOplogEntryMovesIdFirst) {
    auto in = BSON("a" << 1 << "b" << 2 << "_id" << 7);
    auto out = makeReplacementOplogEntry(in);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 7 << "a" << 1 << "b" << 2), out);
}

TEST(UpdateOplogSerializationTest, MakeReplacementOplogEntryNoopWhenIdAlreadyFirst) {
    auto in = BSON("_id" << 7 << "a" << 1 << "b" << 2);
    auto out = makeReplacementOplogEntry(in);
    ASSERT_BSONOBJ_BINARY_EQ(in, out);
}

TEST(UpdateOplogSerializationTest, MakeReplacementOplogEntryNoopWhenIdMissing) {
    auto in = BSON("a" << 1 << "b" << 2);
    auto out = makeReplacementOplogEntry(in);
    ASSERT_BSONOBJ_BINARY_EQ(in, out);
}

TEST(UpdateOplogSerializationTest, MakeReplacementOplogEntryEmpty) {
    auto out = makeReplacementOplogEntry(BSONObj());
    ASSERT_BSONOBJ_BINARY_EQ(BSONObj(), out);
}

TEST(UpdateOplogSerializationTest, ReadV2Entry) {
    auto v2Entry = fromjson(
        "{d: {deletedField1: false, deletedField2: false}, u: {updatedField: 'foo'}, i: "
        "{insertedField: 'bar'}}");
    BSONObj o(makeDeltaOplogEntry(v2Entry));

    ASSERT_BSONELT_EQ(extractNewValueForField(o, "deletedField1"), BSONElement());
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "deletedField2"), BSONElement());
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "updatedField"), v2Entry["u"]["updatedField"]);
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "insertedField"), v2Entry["i"]["insertedField"]);

    ASSERT(isFieldRemovedByUpdate(o, "deletedField1") == FieldRemovedStatus::kFieldRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "deletedField2") == FieldRemovedStatus::kFieldRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "updatedField") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "insertedField") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "nonexistentField") == FieldRemovedStatus::kFieldNotRemoved);
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithSubfieldModified_NotSupported1,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a.b" << 1 << "x" << 2);
    BSONObj o(BSON("$set" << setField));

    // We cannot recover the entire new value for field 'a' so an EOO element is returned.
    extractNewValueForField(o, "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadV1EntryWithSubfieldModified_NotSupported2,
                 "Tripwire assertion.*6448500") {
    auto setField = BSON("a.b" << 1 << "x" << 2);
    BSONObj o(BSON("$set" << setField));

    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
}

TEST(UpdateOplogSerializationTest, ReadV2EntryWithSubfieldModified) {
    auto v2Entry = fromjson("{sa: {i: {b: 1}}}");
    BSONObj o(makeDeltaOplogEntry(v2Entry));

    // We cannot recover the entire new value for field 'a' so an EOO element is returned.
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "a"), BSONElement());
    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadReplacementEntry_NotSupported1,
                 "Tripwire assertion.*6448500") {
    BSONObj o(BSON("foo" << 1 << "bar" << 2));

    isFieldRemovedByUpdate(o, "bar");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 ReadReplacementEntry_NotSupported2,
                 "Tripwire assertion.*6448500") {
    BSONObj o(BSON("foo" << 1 << "bar" << 2));

    extractNewValueForField(o, "foo");
}


DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 CannotExtractDottedField,
                 "cannot contain dots") {
    extractNewValueForField(BSONObj(), "a.b");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 CannotReadDottedField,
                 "cannot contain dots") {
    isFieldRemovedByUpdate(BSONObj(), "a.b");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 CannotExtractFromNonExistentVersion,
                 "Tripwire assertion.*6448500") {
    extractNewValueForField(BSON("$v" << 10), "a");
}

DEATH_TEST_REGEX(UpdateOplogSerializationTestDeathTest,
                 CannotReadNonExistentVersion,
                 "Tripwire assertion.*6448500") {
    isFieldRemovedByUpdate(BSON("$v" << 10), "a");
}
}  // namespace
}  // namespace mongo::update_oplog_entry
