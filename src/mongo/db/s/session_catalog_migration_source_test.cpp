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
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/db/session.h"
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
    migrationSource.init(opCtx());
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWrites) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry1.setStatementId(0);
    entry1.setWallClockTime(Date_t::now());
    insertOplogEntry(entry1);

    repl::OplogEntry entry2(
        repl::OpTime(Timestamp(67, 54801), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("y" << 50));
    entry2.setPrevWriteOpTimeInTransaction(entry1.getOpTime());
    entry2.setStatementId(1);
    entry2.setWallClockTime(Date_t::now());
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry2.toBSON(), nextOplogResult.oplog->toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.toBSON(), nextOplogResult.oplog->toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWrites) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1a(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1a.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry1a.setStatementId(0);
    entry1a.setWallClockTime(Date_t::now());

    repl::OplogEntry entry1b(
        repl::OpTime(Timestamp(67, 54801), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("y" << 50));
    entry1b.setStatementId(1);
    entry1b.setWallClockTime(Date_t::now());
    entry1b.setPrevWriteOpTimeInTransaction(entry1a.getOpTime());

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    repl::OplogEntry entry2a(
        repl::OpTime(Timestamp(43, 12), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("x" << 30));
    entry2a.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry2a.setStatementId(3);
    entry2a.setWallClockTime(Date_t::now());

    repl::OplogEntry entry2b(
        repl::OpTime(Timestamp(789, 13), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("y" << 50));
    entry2b.setPrevWriteOpTimeInTransaction(entry2a.getOpTime());
    entry2b.setStatementId(4);
    entry2b.setWallClockTime(Date_t::now());

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2b.getOpTime());
    sessionRecord2.setLastWriteDate(*entry2b.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    insertOplogEntry(entry2a);
    insertOplogEntry(entry1a);
    insertOplogEntry(entry1b);
    insertOplogEntry(entry2b);

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto checkNextBatch = [this, &migrationSource](const repl::OplogEntry& firstExpectedOplog,
                                                   const repl::OplogEntry& secondExpectedOplog) {
        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            // Cannot compare directly because of SERVER-31356
            ASSERT_BSONOBJ_EQ(firstExpectedOplog.toBSON(), nextOplogResult.oplog->toBSON());
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        }

        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            ASSERT_BSONOBJ_EQ(secondExpectedOplog.toBSON(), nextOplogResult.oplog->toBSON());
        }
    };

    if (sessionRecord1.getSessionId().toBSON().woCompare(sessionRecord2.getSessionId().toBSON()) <
        0) {
        checkNextBatch(entry2b, entry2a);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        checkNextBatch(entry1b, entry1a);

    } else {
        checkNextBatch(entry1b, entry1a);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        checkNextBatch(entry2b, entry2a);
    }
}

// It is currently not possible to have 2 findAndModify operations in one transaction, but this
// will test the oplog buffer more.
TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithFindAndModifyPreImageAndPostImage) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 30));
    entry1.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry1.setStatementId(0);
    entry1.setWallClockTime(Date_t::now());
    insertOplogEntry(entry1);

    repl::OplogEntry entry2(
        repl::OpTime(Timestamp(52, 346), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("y" << 50));
    entry2.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry2.setPreImageOpTime(entry1.getOpTime());
    entry2.setStatementId(1);
    entry2.setWallClockTime(Date_t::now());
    insertOplogEntry(entry2);

    repl::OplogEntry entry3(
        repl::OpTime(Timestamp(73, 5), 2), 0, repl::OpTypeEnum::kNoop, kNs, BSON("x" << 20));
    entry3.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry3.setStatementId(2);
    entry3.setWallClockTime(Date_t::now());
    insertOplogEntry(entry3);

    repl::OplogEntry entry4(repl::OpTime(Timestamp(73, 6), 2),
                            0,
                            repl::OpTypeEnum::kUpdate,
                            kNs,
                            BSON("x" << 19),
                            BSON("$inc" << BSON("x" << 1)));
    entry4.setPrevWriteOpTimeInTransaction(entry2.getOpTime());
    entry4.setPostImageOpTime(entry3.getOpTime());
    entry4.setStatementId(3);
    entry4.setWallClockTime(Date_t::now());
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry4.getOpTime());
    sessionRecord.setLastWriteDate(*entry4.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequece = {entry3, entry4, entry1, entry2};

    for (auto oplog : expectedSequece) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.toBSON(), nextOplogResult.oplog->toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OplogWithOtherNsShouldBeIgnored) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry1.setStatementId(0);
    entry1.setWallClockTime(Date_t::now());
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());


    repl::OplogEntry entry2(repl::OpTime(Timestamp(53, 12), 2),
                            0,
                            repl::OpTypeEnum::kDelete,
                            NamespaceString("x.y"),
                            BSON("x" << 30));
    entry2.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry2.setStatementId(1);
    entry2.setWallClockTime(Date_t::now());
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord2.setLastWriteDate(*entry2.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(entry1.toBSON(), nextOplogResult.oplog->toBSON());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, SessionDumpWithMultipleNewWrites) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry1(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry1.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry1.setStatementId(0);
    entry1.setWallClockTime(Date_t::now());
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    repl::OplogEntry entry2(
        repl::OpTime(Timestamp(53, 12), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("x" << 30));
    entry2.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry2.setStatementId(1);
    entry2.setWallClockTime(Date_t::now());
    insertOplogEntry(entry2);

    repl::OplogEntry entry3(
        repl::OpTime(Timestamp(55, 12), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("z" << 40));
    entry3.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
    entry3.setStatementId(2);
    entry3.setWallClockTime(Date_t::now());
    insertOplogEntry(entry3);

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteOpTime(entry2.getOpTime());
    migrationSource.notifyNewWriteOpTime(entry3.getOpTime());

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.toBSON(), nextOplogResult.oplog->toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry2.toBSON(), nextOplogResult.oplog->toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry3.toBSON(), nextOplogResult.oplog->toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldAssertIfOplogCannotBeFound) {
    const NamespaceString kNs("a.b");

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteOpTime(repl::OpTime(Timestamp(100, 3), 1));
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldBeAbleInsertNewWritesAfterBufferWasDepleted) {
    const NamespaceString kNs("a.b");

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    {
        repl::OplogEntry entry(repl::OpTime(Timestamp(52, 345), 2),
                               0,
                               repl::OpTypeEnum::kInsert,
                               kNs,
                               BSON("x" << 30));
        entry.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
        entry.setStatementId(0);
        entry.setWallClockTime(Date_t::now());
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(entry.getOpTime());

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextOplogResult.oplog->toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        repl::OplogEntry entry(
            repl::OpTime(Timestamp(53, 12), 2), 0, repl::OpTypeEnum::kDelete, kNs, BSON("x" << 30));
        entry.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
        entry.setStatementId(1);
        entry.setWallClockTime(Date_t::now());
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(entry.getOpTime());

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextOplogResult.oplog->toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        repl::OplogEntry entry(
            repl::OpTime(Timestamp(55, 12), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("z" << 40));
        entry.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(0, 0), 0));
        entry.setStatementId(2);
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(entry.getOpTime());

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextOplogResult.oplog->toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }
}

TEST_F(SessionCatalogMigrationSourceTest, ReturnsDeadEndSentinelForIncompleteHistory) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(40, 1), 2));
    entry.setStatementId(0);
    entry.setWallClockTime(Date_t::now());
    insertOplogEntry(entry);

    const auto sessionId = makeLogicalSessionIdForTest();

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(31);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(*entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextOplogResult.oplog->toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);

        auto oplog = *nextOplogResult.oplog;
        ASSERT_TRUE(oplog.getObject2());
        ASSERT_BSONOBJ_EQ(Session::kDeadEndSentinel, *oplog.getObject2());
        ASSERT_TRUE(oplog.getStatementId());
        ASSERT_EQ(kIncompleteHistoryStmtId, *oplog.getStatementId());

        auto sessionInfo = oplog.getOperationSessionInfo();
        ASSERT_TRUE(sessionInfo.getSessionId());
        ASSERT_EQ(sessionId, *sessionInfo.getSessionId());
        ASSERT_TRUE(sessionInfo.getTxnNumber());
        ASSERT_EQ(31, *sessionInfo.getTxnNumber());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldAssertWhenRollbackDetected) {
    const NamespaceString kNs("a.b");

    repl::OplogEntry entry(
        repl::OpTime(Timestamp(52, 345), 2), 0, repl::OpTypeEnum::kInsert, kNs, BSON("x" << 30));
    entry.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp(40, 1), 2));
    entry.setStatementId(0);
    entry.setWallClockTime(Date_t::now());
    insertOplogEntry(entry);

    const auto sessionId = makeLogicalSessionIdForTest();

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(31);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(*entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(kNs);
    migrationSource.init(opCtx());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextOplogResult.oplog->toBSON());
    }

    ASSERT_OK(repl::ReplicationProcess::get(opCtx())->incrementRollbackID(opCtx()));

    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
}

}  // namespace
}  // namespace mongo
