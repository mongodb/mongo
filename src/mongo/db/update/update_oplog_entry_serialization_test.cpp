/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::update_oplog_entry {
namespace {

TEST(UpdateOplogSerializationTest, ReadV1EntryWithVersionField) {
    auto setField = BSON("a" << 1 << "b" << 2);
    BSONObj o(BSON("$v" << 1 << "$set" << setField));

    ASSERT_BSONELT_EQ(extractNewValueForField(o, "a"), setField["a"]);
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "b"), setField["b"]);

    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "z") == FieldRemovedStatus::kFieldNotRemoved);
}

TEST(UpdateOplogSerializationTest, ReadV1EntryWithoutVersionField) {
    auto setField = BSON("a" << 1 << "b" << 2);
    BSONObj o(BSON("$set" << setField));

    ASSERT_BSONELT_EQ(extractNewValueForField(o, "a"), setField["a"]);
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "b"), setField["b"]);

    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "z") == FieldRemovedStatus::kFieldNotRemoved);
}

TEST(UpdateOplogSerializationTest, ReadV1EntryWithSetAndUnset) {
    auto setField = BSON("a" << 1 << "b" << 2);
    auto unsetField = BSON("c" << true << "d" << true);
    BSONObj o(BSON("$set" << setField << "$unset" << unsetField));

    ASSERT_BSONELT_EQ(extractNewValueForField(o, "a"), setField["a"]);
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "b"), setField["b"]);
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "c"), BSONElement());
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "d"), BSONElement());

    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "b") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "c") == FieldRemovedStatus::kFieldRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "d") == FieldRemovedStatus::kFieldRemoved);
}

TEST(UpdateOplogSerializationTest, ReadV1EntryWhichIncludesDottedPath) {
    // While our function for getting modified fields only supports top-level fields,
    // there should be no problem if the oplog entry contains modifications to
    // dotted paths.

    auto setField = BSON("a.b.c" << 1 << "x" << 2);
    auto unsetField = BSON("d.e.f" << true << "y" << true);
    BSONObj o(BSON("$set" << setField << "$unset" << unsetField));

    ASSERT_BSONELT_EQ(extractNewValueForField(o, "x"), setField["x"]);
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "y"), BSONElement());

    ASSERT(isFieldRemovedByUpdate(o, "x") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "y") == FieldRemovedStatus::kFieldRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "z") == FieldRemovedStatus::kFieldNotRemoved);
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

TEST(UpdateOplogSerializationTest, ReadV1EntryWithSubfieldModified) {
    auto setField = BSON("a.b" << 1 << "x" << 2);
    BSONObj o(BSON("$set" << setField));

    // We cannot recover the entire new value for field 'a' so an EOO element is returned.
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "a"), BSONElement());
    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
}

TEST(UpdateOplogSerializationTest, ReadV2EntryWithSubfieldModified) {
    auto v2Entry = fromjson("{sa: {i: {b: 1}}}");
    BSONObj o(makeDeltaOplogEntry(v2Entry));

    // We cannot recover the entire new value for field 'a' so an EOO element is returned.
    ASSERT_BSONELT_EQ(extractNewValueForField(o, "a"), BSONElement());
    ASSERT(isFieldRemovedByUpdate(o, "a") == FieldRemovedStatus::kFieldNotRemoved);
}

TEST(UpdateOplogSerializationTest, ReadReplacementEntry) {
    BSONObj o(BSON("foo" << 1 << "bar" << 2));

    ASSERT_EQ(extractNewValueForField(o, "foo"), o["foo"]);
    ASSERT_EQ(extractNewValueForField(o, "bar"), o["bar"]);

    ASSERT(isFieldRemovedByUpdate(o, "bar") == FieldRemovedStatus::kFieldNotRemoved);
    ASSERT(isFieldRemovedByUpdate(o, "baz") == FieldRemovedStatus::kUnknown);
}

DEATH_TEST(UpdateOplogSerializationTest, CannotExtractDottedField, "cannot contain dots") {
    extractNewValueForField(BSONObj(), "a.b");
}

DEATH_TEST(UpdateOplogSerializationTest, CannotReadDottedField, "cannot contain dots") {
    isFieldRemovedByUpdate(BSONObj(), "a.b");
}

DEATH_TEST(UpdateOplogSerializationTest, CannotExtractFromNonExistentVersion, "Unrecognized") {
    extractNewValueForField(BSON("$v" << 10), "a");
}

DEATH_TEST(UpdateOplogSerializationTest, CannotReadNonExistentVersion, "Unrecognized") {
    isFieldRemovedByUpdate(BSON("$v" << 10), "a");
}
}  // namespace
}  // namespace mongo::update_oplog_entry
