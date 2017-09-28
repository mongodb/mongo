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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using executor::RemoteCommandRequest;

namespace {

class SessionCatalogMigrationSourceTest : public MockReplCoordServerFixture {};

TEST_F(SessionCatalogMigrationSourceTest, NoSessionsToTransferShouldNotHaveOplog) {
    const NamespaceString kNs("a.b");
    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWrites) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry1);

    repl::OplogEntry entry2(
        repl::OpTime(Timestamp(67, 54801), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("y" << 50));
    entry2.setPrevWriteTsInTransaction(entry1.getTimestamp());
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTimeTs(entry2.getTimestamp());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry2.toBSON(), nextDoc);
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry1.toBSON(), nextDoc);
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWrites) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1a(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1a.setPrevWriteTsInTransaction(Timestamp(0, 0));

    repl::OplogEntry entry1b(
        repl::OpTime(Timestamp(67, 54801), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("y" << 50));
    entry1b.setPrevWriteTsInTransaction(entry1a.getTimestamp());

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTimeTs(entry1b.getTimestamp());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());


    repl::OplogEntry entry2a(
        repl::OpTime(Timestamp(43, 12), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("x" << 30));
    entry2a.setPrevWriteTsInTransaction(Timestamp(0, 0));

    repl::OplogEntry entry2b(
        repl::OpTime(Timestamp(789, 13), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("y" << 50));
    entry2b.setPrevWriteTsInTransaction(entry2a.getTimestamp());

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTimeTs(entry2b.getTimestamp());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    insertOplogEntry(entry2a);
    insertOplogEntry(entry1a);
    insertOplogEntry(entry1b);
    insertOplogEntry(entry2b);

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto checkNextBatch = [this, &migrationSource](const repl::OplogEntry& firstExpectedOplog,
                                                   const repl::OplogEntry& secondExpectedOplog) {
        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextDoc = migrationSource.getLastFetchedOplog();
            ASSERT_BSONOBJ_EQ(firstExpectedOplog.toBSON(), nextDoc);
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        }

        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextDoc = migrationSource.getLastFetchedOplog();
            ASSERT_BSONOBJ_EQ(secondExpectedOplog.toBSON(), nextDoc);
        }
    };

    if (sessionRecord1.getSessionId().toBSON().woCompare(sessionRecord2.getSessionId().toBSON()) <
        0) {
        checkNextBatch(entry1b, entry1a);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        checkNextBatch(entry2b, entry2a);
    } else {
        checkNextBatch(entry2b, entry2a);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        checkNextBatch(entry1b, entry1a);
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

// It is currently not possible to have 2 findAndModify operations in one transaction, but this
// will test the oplog buffer more.
TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithFindAndModifyPreImageAndPostImage) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 30));
    entry1.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry1);

    repl::OplogEntry entry2(
        repl::OpTime(Timestamp(52, 346), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("y" << 50));
    entry2.setPrevWriteTsInTransaction(Timestamp(0, 0));
    entry2.setPreImageTs(entry1.getTimestamp());
    insertOplogEntry(entry2);

    repl::OplogEntry entry3(
        repl::OpTime(Timestamp(73, 5), 2), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 20));
    entry3.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry3);

    repl::OplogEntry entry4(repl::OpTime(Timestamp(73, 6), 2),
                            0,
                            repl::OpTypeEnum::kUpdate,
                            kNs,
                            BSON("x" << 19),
                            BSON("$inc" << BSON("x" << 1)));
    entry4.setPrevWriteTsInTransaction(entry2.getTimestamp());
    entry4.setPostImageTs(entry3.getTimestamp());
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTimeTs(entry4.getTimestamp());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequece = {entry3.toBSON(), entry4.toBSON(), entry1.toBSON(), entry2.toBSON()};

    for (auto oplogDoc : expectedSequece) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(oplogDoc, nextDoc);
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OplogWithOtherNsShouldBeIgnored) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTimeTs(entry1.getTimestamp());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());


    repl::OplogEntry entry2(repl::OpTime(Timestamp(53, 12), 2),
                            0,
                            repl::OpTypeEnum::kDelete,
                            NamespaceString("x.y"),
                            BSON("x" << 30));
    entry2.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTimeTs(entry2.getTimestamp());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    auto nextDoc = migrationSource.getLastFetchedOplog();
    ASSERT_BSONOBJ_EQ(entry1.toBSON(), nextDoc);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, SessionDumpWithMultipleNewWrites) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTimeTs(entry1.getTimestamp());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    repl::OplogEntry entry2(
        repl::OpTime(Timestamp(53, 12), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("x" << 30));
    entry2.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry2);

    repl::OplogEntry entry3(
        repl::OpTime(Timestamp(55, 12), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("z" << 40));
    entry3.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(entry3);

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteTS(entry2.getTimestamp());
    migrationSource.notifyNewWriteTS(entry3.getTimestamp());

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry1.toBSON(), nextDoc);
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry2.toBSON(), nextDoc);
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry3.toBSON(), nextDoc);
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldAssertIfOplogCannotBeFound) {
    const NamespaceString kNs("a.b");

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteTS(Timestamp(100, 3));
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldBeAbleInsertNewWritesAfterBufferWasDepleted) {
    const NamespaceString kNs("a.b");

    SessionCatalogMigrationSource migrationSource(kNs);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    {
        repl::OplogEntry entry(repl::OpTime(Timestamp(52, 345), 2),
                               0,
                               repl::OpTypeEnum::kInsert,
                               kNs,
                               BSON("x" << 30));
        entry.setPrevWriteTsInTransaction(Timestamp(0, 0));
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteTS(entry.getTimestamp());

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextDoc);

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        repl::OplogEntry entry(
            repl::OpTime(Timestamp(53, 12), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("x" << 30));
        entry.setPrevWriteTsInTransaction(Timestamp(0, 0));
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteTS(entry.getTimestamp());

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextDoc);

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        repl::OplogEntry entry(
            repl::OpTime(Timestamp(55, 12), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("z" << 40));
        entry.setPrevWriteTsInTransaction(Timestamp(0, 0));
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteTS(entry.getTimestamp());

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextDoc = migrationSource.getLastFetchedOplog();
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextDoc);

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }
}

}  // namespace

}  // namespace mongo
