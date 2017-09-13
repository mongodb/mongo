/**
 *    Copyright (C) 2017 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using SessionHistoryIteratorTest = MockReplCoordServerFixture;

TEST_F(SessionHistoryIteratorTest, NormalHistory) {
    repl::OplogEntry entry1(repl::OpTime(Timestamp(52, 345), 2),
                            0,
                            repl::OpTypeEnum::kInsert,
                            NamespaceString("a.b"),
                            BSON("x" << 30));
    entry1.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry1);

    repl::OplogEntry entry2(repl::OpTime(Timestamp(67, 54801), 2),
                            0,
                            repl::OpTypeEnum::kInsert,
                            NamespaceString("a.b"),
                            BSON("y" << 50));
    entry2.setPrevWriteTsInTransaction(Timestamp(52, 345));
    insertOplogEntry(entry2);

    // Insert an unrelated entry in between
    repl::OplogEntry entry3(repl::OpTime(Timestamp(83, 2), 2),
                            0,
                            repl::OpTypeEnum::kInsert,
                            NamespaceString("a.b"),
                            BSON("z" << 40));
    entry3.setPrevWriteTsInTransaction(Timestamp(22, 67));
    insertOplogEntry(entry3);

    repl::OplogEntry entry4(repl::OpTime(Timestamp(97, 2472), 2),
                            0,
                            repl::OpTypeEnum::kInsert,
                            NamespaceString("a.b"),
                            BSON("a" << 3));
    entry4.setPrevWriteTsInTransaction(Timestamp(67, 54801));
    insertOplogEntry(entry4);

    TransactionHistoryIterator iter(Timestamp(97, 2472));

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(Timestamp(97, 2472), nextEntry.getTimestamp());
        ASSERT_BSONOBJ_EQ(BSON("a" << 3), nextEntry.getObject());
    }

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(Timestamp(67, 54801), nextEntry.getTimestamp());
        ASSERT_BSONOBJ_EQ(BSON("y" << 50), nextEntry.getObject());
    }

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(Timestamp(52, 345), nextEntry.getTimestamp());
        ASSERT_BSONOBJ_EQ(BSON("x" << 30), nextEntry.getObject());
    }

    ASSERT_FALSE(iter.hasNext());
}

TEST_F(SessionHistoryIteratorTest, StartAtZeroTSShouldNotBeAbleToIterate) {
    repl::OplogEntry entry(repl::OpTime(Timestamp(67, 54801), 2),
                           0,
                           repl::OpTypeEnum::kInsert,
                           NamespaceString("a.b"),
                           BSON("y" << 50));
    entry.setPrevWriteTsInTransaction(Timestamp(52, 345));
    insertOplogEntry(entry);

    TransactionHistoryIterator iter(Timestamp(0, 0));
    ASSERT_FALSE(iter.hasNext());
}

TEST_F(SessionHistoryIteratorTest, NextShouldAssertIfHistoryIsTruncated) {
    repl::OplogEntry entry(repl::OpTime(Timestamp(67, 54801), 2),
                           0,
                           repl::OpTypeEnum::kInsert,
                           NamespaceString("a.b"),
                           BSON("y" << 50));
    entry.setPrevWriteTsInTransaction(Timestamp(52, 345));
    insertOplogEntry(entry);

    TransactionHistoryIterator iter(Timestamp(67, 54801));
    ASSERT_TRUE(iter.hasNext());

    auto nextEntry = iter.next(opCtx());
    ASSERT_EQ(Timestamp(67, 54801), nextEntry.getTimestamp());
    ASSERT_BSONOBJ_EQ(BSON("y" << 50), nextEntry.getObject());

    ASSERT_TRUE(iter.hasNext());
    ASSERT_THROWS_CODE(
        iter.next(opCtx()), AssertionException, ErrorCodes::IncompleteTransactionHistory);
}

TEST_F(SessionHistoryIteratorTest, OplogInWriteHistoryChainWithMissingPrevTSShouldAssert) {
    repl::OplogEntry entry(repl::OpTime(Timestamp(67, 54801), 2),
                           0,
                           repl::OpTypeEnum::kInsert,
                           NamespaceString("a.b"),
                           BSON("y" << 50));
    insertOplogEntry(entry);

    TransactionHistoryIterator iter(Timestamp(67, 54801));
    ASSERT_TRUE(iter.hasNext());
    ASSERT_THROWS_CODE(iter.next(opCtx()), AssertionException, ErrorCodes::FailedToParse);
}

}  // namespace mongo
