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

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using SessionHistoryIteratorTest = MockReplCoordServerFixture;

namespace {

/**
 * Creates an OplogEntry with defaults specific to this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                BSONObj docToInsert,
                                boost::optional<repl::OpTime> prevWriteOpTimeInTransaction) {
    return {repl::DurableOplogEntry(
        opTime,                           // optime
        repl::OpTypeEnum::kInsert,        // opType
        NamespaceString("a.b"),           // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        docToInsert,                      // o
        boost::none,                      // o2
        {},                               // sessionInfo
        boost::none,                      // upsert
        Date_t(),                         // wall clock time
        {},                               // statement ids
        prevWriteOpTimeInTransaction,     // optime of previous write within same transaction
        boost::none,                      // pre-image optime
        boost::none,                      // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        boost::none)};                    // needsRetryImage
}

}  // namespace

TEST_F(SessionHistoryIteratorTest, NormalHistory) {
    auto entry1 = makeOplogEntry(repl::OpTime(Timestamp(52, 345), 2),  // optime
                                 BSON("x" << 30),                      // o
                                 repl::OpTime());  //  optime of previous write in transaction

    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(67, 54801), 2),  // optime
        BSON("y" << 50),                        // o
        repl::OpTime(Timestamp(52, 345), 2));   // optime of previous write in transaction
    insertOplogEntry(entry2);

    // Insert an unrelated entry in between
    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(83, 2), 2),    // optime
        BSON("z" << 40),                      // o
        repl::OpTime(Timestamp(22, 67), 2));  // optime of previous write in transaction
    insertOplogEntry(entry3);

    auto entry4 = makeOplogEntry(
        repl::OpTime(Timestamp(97, 2472), 2),    // optime
        BSON("a" << 3),                          // o
        repl::OpTime(Timestamp(67, 54801), 2));  // optime of previous write in transaction
    insertOplogEntry(entry4);

    TransactionHistoryIterator iter(repl::OpTime(Timestamp(97, 2472), 2));

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(repl::OpTime(Timestamp(97, 2472), 2), nextEntry.getOpTime());
        ASSERT_BSONOBJ_EQ(BSON("a" << 3), nextEntry.getObject());
    }

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(repl::OpTime(Timestamp(67, 54801), 2), nextEntry.getOpTime());
        ASSERT_BSONOBJ_EQ(BSON("y" << 50), nextEntry.getObject());
    }

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(repl::OpTime(Timestamp(52, 345), 2), nextEntry.getOpTime());
        ASSERT_BSONOBJ_EQ(BSON("x" << 30), nextEntry.getObject());
    }

    ASSERT_FALSE(iter.hasNext());
}

TEST_F(SessionHistoryIteratorTest, StartAtZeroTSShouldNotBeAbleToIterate) {
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(67, 54801), 2),  // optime
        BSON("y" << 50),                        // o
        repl::OpTime(Timestamp(52, 345), 1));   // optime of previous write in transaction
    insertOplogEntry(entry);

    TransactionHistoryIterator iter({});
    ASSERT_FALSE(iter.hasNext());
}

TEST_F(SessionHistoryIteratorTest, NextShouldAssertIfHistoryIsTruncated) {
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(67, 54801), 2),  // optime
        BSON("y" << 50),                        // o
        repl::OpTime(Timestamp(52, 345), 1));   // optime of previous write in transaction
    insertOplogEntry(entry);

    repl::OpTime opTime(Timestamp(67, 54801), 2);
    TransactionHistoryIterator iter(opTime);
    ASSERT_TRUE(iter.hasNext());

    auto nextEntry = iter.next(opCtx());
    ASSERT_EQ(opTime, nextEntry.getOpTime());
    ASSERT_BSONOBJ_EQ(BSON("y" << 50), nextEntry.getObject());

    ASSERT_TRUE(iter.hasNext());
    ASSERT_THROWS_CODE(
        iter.next(opCtx()), AssertionException, ErrorCodes::IncompleteTransactionHistory);
}

TEST_F(SessionHistoryIteratorTest, OplogInWriteHistoryChainWithMissingPrevTSShouldAssert) {
    auto entry = makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                                BSON("y" << 50),                        // o
                                boost::none);  // optime of previous write in transaction
    insertOplogEntry(entry);

    TransactionHistoryIterator iter(repl::OpTime(Timestamp(67, 54801), 2));
    ASSERT_TRUE(iter.hasNext());
    ASSERT_THROWS_CODE(iter.next(opCtx()), AssertionException, ErrorCodes::FailedToParse);
}

}  // namespace mongo
