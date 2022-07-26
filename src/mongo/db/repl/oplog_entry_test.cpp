/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

const OpTime entryOpTime{Timestamp(3, 4), 5};
const NamespaceString nss{"foo", "bar"};
const int docId = 17;

TEST(OplogEntryTest, Update) {
    const BSONObj doc = BSON("_id" << docId);
    const BSONObj update = BSON("$set" << BSON("a" << 4));
    const auto entry = makeUpdateDocumentOplogEntry(entryOpTime, nss, doc, update);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), doc);
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), update);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, Insert) {
    const BSONObj doc = BSON("_id" << docId << "a" << 5);
    const auto entry = makeInsertDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), BSON("_id" << docId));
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, Delete) {
    const BSONObj doc = BSON("_id" << docId);
    const auto entry = makeDeleteDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), doc);
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, Create) {
    CollectionOptions opts;
    opts.capped = true;
    opts.cappedSize = 15;

    const auto entry = makeCreateCollectionOplogEntry(entryOpTime, nss, opts.toBSON());

    ASSERT(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT_FALSE(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(),
                      BSON("create" << nss.coll() << "capped" << true << "size" << 15));
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kCreate);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, OpTimeBaseNonStrictParsing) {
    const BSONObj oplogEntryExtraField = BSON("ts" << Timestamp(0, 0) << "t" << 0LL << "op"
                                                   << "c"
                                                   << "ns" << nss.ns() << "wall" << Date_t() << "o"
                                                   << BSON("_id" << 1) << "extraField" << 3);

    // OpTimeBase should be successfully created from an OplogEntry, even though it has
    // extraneous fields.
    UNIT_TEST_INTERNALS_IGNORE_UNUSED_RESULT_WARNINGS(
        OpTimeBase::parse(IDLParserContext("OpTimeBase"), oplogEntryExtraField));

    // OplogEntryBase should still use strict parsing and throw an error when it has extraneous
    // fields.
    ASSERT_THROWS_CODE(
        OplogEntryBase::parse(IDLParserContext("OplogEntryBase"), oplogEntryExtraField),
        AssertionException,
        40415);

    const BSONObj oplogEntryMissingTimestamp =
        BSON("t" << 0LL << "op"
                 << "c"
                 << "ns" << nss.ns() << "wall" << Date_t() << "o" << BSON("_id" << 1));

    // When an OplogEntryBase is created with a missing required field in a chained struct, it
    // should throw an exception.
    ASSERT_THROWS_CODE(
        OplogEntryBase::parse(IDLParserContext("OplogEntryBase"), oplogEntryMissingTimestamp),
        AssertionException,
        40414);
}

TEST(OplogEntryTest, InsertIncludesTidField) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    const BSONObj doc = BSON("_id" << docId << "a" << 5);
    TenantId tid(OID::gen());
    NamespaceString nss(tid, "foo", "bar");
    const auto entry =
        makeOplogEntry(entryOpTime, OpTypeEnum::kInsert, nss, doc, boost::none, {}, Date_t::now());

    ASSERT(entry.getTid());
    ASSERT_EQ(*entry.getTid(), tid);
    // TODO SERVER-66708 Check that (entry.getNss() == nss) once the OplogEntry deserializer
    // passes "tid" to the NamespaceString constructor
    ASSERT_EQ(entry.getNss(), NamespaceString(boost::none, nss.ns()));
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), BSON("_id" << docId));
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
