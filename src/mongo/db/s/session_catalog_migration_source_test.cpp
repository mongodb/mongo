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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const NamespaceString kNs("a.b");
const KeyPattern kShardKey(BSON("x" << 1));
const ChunkRange kChunkRange(BSON("x" << 0), BSON("x" << 100));

class SessionCatalogMigrationSourceTest : public MockReplCoordServerFixture {};

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2,
                                boost::optional<Date_t> wallClockTime,
                                StmtId stmtId,
                                repl::OpTime prevWriteOpTimeInTransaction,
                                boost::optional<repl::OpTime> preImageOpTime,
                                boost::optional<repl::OpTime> postImageOpTime) {
    return repl::OplogEntry(
        opTime,                           // optime
        0,                                // hash
        opType,                           // opType
        nss,                              // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        object,                           // o
        object2,                          // o2
        {},                               // sessionInfo
        boost::none,                      // upsert
        wallClockTime,                    // wall clock time
        stmtId,                           // statement id
        prevWriteOpTimeInTransaction,     // optime of previous write within same transaction
        preImageOpTime,                   // pre-image optime
        postImageOpTime);                 // post-image optime
}

repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                BSONObj object,
                                boost::optional<BSONObj> object2,
                                boost::optional<Date_t> wallClockTime,
                                StmtId stmtId,
                                repl::OpTime prevWriteOpTimeInTransaction,
                                boost::optional<repl::OpTime> preImageOpTime = boost::none,
                                boost::optional<repl::OpTime> postImageOpTime = boost::none) {
    return makeOplogEntry(opTime,
                          opType,
                          kNs,
                          object,
                          object2,
                          wallClockTime,
                          stmtId,
                          prevWriteOpTimeInTransaction,
                          preImageOpTime,
                          postImageOpTime);
}

TEST_F(SessionCatalogMigrationSourceTest, NoSessionsToTransferShouldNotHaveOplog) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWrites) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       1,                                      // statement id
                       entry1.getOpTime());  // optime of previous write within same transaction
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
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
    auto entry1a = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    auto entry1b =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       1,                                      // statement id
                       entry1a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2a = makeOplogEntry(
        repl::OpTime(Timestamp(43, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        3,                                   // statement id
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction

    auto entry2b =
        makeOplogEntry(repl::OpTime(Timestamp(789, 13), 2),  // optime
                       repl::OpTypeEnum::kDelete,            // op type
                       BSON("x" << 50),                      // o
                       boost::none,                          // o2
                       Date_t::now(),                        // wall clock time
                       4,                                    // statement id
                       entry2a.getOpTime());  // optime of previous write within same transaction

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

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
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
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        1,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(73, 5), 2),  // optime
        repl::OpTypeEnum::kNoop,            // op type
        BSON("x" << 20),                    // o
        boost::none,                        // o2
        Date_t::now(),                      // wall clock time
        2,                                  // statement id
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry3);

    auto entry4 =
        makeOplogEntry(repl::OpTime(Timestamp(73, 6), 2),  // optime
                       repl::OpTypeEnum::kUpdate,          // op type
                       BSON("$inc" << BSON("x" << 1)),     // o
                       BSON("x" << 19),                    // o2
                       Date_t::now(),                      // wall clock time
                       3,                                  // statement id
                       entry2.getOpTime(),   // optime of previous write within same transaction
                       boost::none,          // pre-image optime
                       entry3.getOpTime());  // post-image optime
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry4.getOpTime());
    sessionRecord.setLastWriteDate(*entry4.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
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
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());


    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(53, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        NamespaceString("x.y"),              // namespace
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        1,                                   // statement id
        repl::OpTime(Timestamp(0, 0), 0),    // optime of previous write within same transaction
        boost::none,                         // pre-image optime
        boost::none);                        // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord2.setLastWriteDate(*entry2.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
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
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(53, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        1,                                   // statement id
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(55, 12), 2),  // optime
        repl::OpTypeEnum::kInsert,           // op type
        BSON("x" << 40),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        2,                                   // statement id
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
    insertOplogEntry(entry3);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteOpTime(
        entry2.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);
    migrationSource.notifyNewWriteOpTime(
        entry3.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

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
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteOpTime(
        repl::OpTime(Timestamp(100, 3), 1),
        SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldBeAbleInsertNewWritesAfterBufferWasDepleted) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    {
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(52, 345), 2),  // optime
            repl::OpTypeEnum::kInsert,            // op type
            BSON("x" << 30),                      // o
            boost::none,                          // o2
            Date_t::now(),                        // wall clock time
            0,                                    // statement id
            repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

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
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(53, 12), 2),  // optime
            repl::OpTypeEnum::kDelete,           // op type
            BSON("x" << 30),                     // o
            boost::none,                         // o2
            Date_t::now(),                       // wall clock time
            1,                                   // statement id
            repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.toBSON(), nextOplogResult.oplog->toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(55, 12), 2),  // optime
            repl::OpTypeEnum::kInsert,           // op type
            BSON("x" << 40),                     // o
            boost::none,                         // o2
            Date_t::now(),                       // wall clock time
            2,                                   // statement id
            repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

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
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(40, 1), 2));   // optime of previous write within same transaction
    insertOplogEntry(entry);

    const auto sessionId = makeLogicalSessionIdForTest();

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(31);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(*entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
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
        ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel, *oplog.getObject2());
        ASSERT_TRUE(oplog.getStatementId());
        ASSERT_EQ(kIncompleteHistoryStmtId, *oplog.getStatementId());
        ASSERT_TRUE(oplog.getWallClockTime());
        ASSERT_NE(Date_t{}, *oplog.getWallClockTime());

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
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(40, 1), 2));   // optime of previous write within same transaction
    insertOplogEntry(entry);

    const auto sessionId = makeLogicalSessionIdForTest();

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(31);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(*entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
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

TEST_F(SessionCatalogMigrationSourceTest,
       CommitTransactionEntriesShouldBeConvertedToDeadEndSentinel) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kCommitted);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.hasMoreOplog());

    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel,
                      *nextOplogResult.oplog->getObject2());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       PrepareTransactionEntriesShouldBeConvertedToDeadEndSentinel) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kPrepared);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.hasMoreOplog());

    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel,
                      *nextOplogResult.oplog->getObject2());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, InProgressTransactionEntriesShouldBeIgnored) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kInProgress);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, AbortedTransactionEntriesShouldBeIgnored) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kAborted);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       MixedTransactionEntriesAndRetryableWritesEntriesReturnCorrectResults) {
    // Create an entry corresponding to a retryable write.
    auto insertOplog = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    // Create a config.transaction entry pointing to the insert oplog entry.
    SessionTxnRecord retryableWriteRecord;
    retryableWriteRecord.setSessionId(makeLogicalSessionIdForTest());
    retryableWriteRecord.setTxnNum(1);
    retryableWriteRecord.setLastWriteOpTime(insertOplog.getOpTime());
    retryableWriteRecord.setLastWriteDate(*insertOplog.getWallClockTime());

    // Create a config.transaction entry pointing to an imaginary commitTransaction entry.
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(1);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 2));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kCommitted);

    // Insert both entries into the config.transactions table.
    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  retryableWriteRecord.toBSON());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    // Insert the 'insert' oplog entry into the oplog.
    insertOplogEntry(insertOplog);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    // Function to verify the oplog entry corresponding to the retryable write.
    auto checkRetryableWriteEntry = [&] {
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(insertOplog.toBSON(), nextOplogResult.oplog->toBSON());
    };

    // Function to verify the oplog entry corresponding to the transaction.
    auto checkTxnEntry = [&] {
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel,
                          *nextOplogResult.oplog->getObject2());
    };

    // Logical session ids are generated randomly and the migration source queries in order of
    // logical session id, so we need to change the order of the checks depending on the ordering of
    // the lsids between the retryable write record and the transaction record.
    if (retryableWriteRecord.getSessionId().toBSON().woCompare(txnRecord.getSessionId().toBSON()) <
        0) {
        checkTxnEntry();
        checkRetryableWriteEntry();
    } else {
        checkRetryableWriteEntry();
        checkTxnEntry();
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyDeleteNotTouchingChunkIsIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        1,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyUpdatePrePostNotTouchingChunkIsIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -5),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("y" << 1)),       // o
        BSON("x" << -5),                      // o2
        Date_t::now(),                        // wall clock time
        1,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       UpdatePreImageTouchingPostNotTouchingChunkShouldNotBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("x" << -50)),     // o
        BSON("x" << 10),                      // o2
        Date_t::now(),                        // wall clock time
        1,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        entry1.getOpTime());                  // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequece = {entry1, entry2};

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

TEST_F(SessionCatalogMigrationSourceTest,
       UpdatePreImageNotTouchingPostTouchingChunkShouldBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("x" << 50)),      // o
        BSON("x" << -10),                     // o2
        Date_t::now(),                        // wall clock time
        1,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        entry1.getOpTime());                  // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyUpdateNotTouchingChunkShouldBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -10 << "y" << 50),        // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("y" << 50)),      // o
        BSON("x" << -10),                     // o2
        Date_t::now(),                        // wall clock time
        1,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        entry1.getOpTime());                  // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(*entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWritesContainingWriteNotInChunk) {
    auto sessionId1 = makeLogicalSessionIdForTest();
    auto sessionId2 = makeLogicalSessionIdForTest();

    auto cmpResult = sessionId1.toBSON().woCompare(sessionId2.toBSON());
    auto lowerSessionId = (cmpResult < 0) ? sessionId1 : sessionId2;
    auto higherSessionId = (cmpResult < 0) ? sessionId2 : sessionId1;

    auto entry1a = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        0,                                    // statement id
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    auto entry1b =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << -50),                       // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       1,                                      // statement id
                       entry1a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(higherSessionId);
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(*entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2a = makeOplogEntry(
        repl::OpTime(Timestamp(43, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        3,                                   // statement id
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction

    auto entry2b =
        makeOplogEntry(repl::OpTime(Timestamp(789, 13), 2),  // optime
                       repl::OpTypeEnum::kDelete,            // op type
                       BSON("x" << 50),                      // o
                       boost::none,                          // o2
                       Date_t::now(),                        // wall clock time
                       4,                                    // statement id
                       entry2a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(lowerSessionId);
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2b.getOpTime());
    sessionRecord2.setLastWriteDate(*entry2b.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    insertOplogEntry(entry2a);
    insertOplogEntry(entry1a);
    insertOplogEntry(entry1b);
    insertOplogEntry(entry2b);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequece = {entry1a, entry2b, entry2a};

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

}  // namespace
}  // namespace mongo
