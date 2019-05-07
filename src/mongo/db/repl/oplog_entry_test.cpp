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
}


}  // namespace
}  // namespace repl
}  // namespace mongo
