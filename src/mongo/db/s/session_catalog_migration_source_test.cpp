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

#include <algorithm>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const NamespaceString kNs("a.b");
const NamespaceString kOtherNs("a.b.c");
const KeyPattern kShardKey(BSON("x" << 1));
const ChunkRange kChunkRange(BSON("x" << 0), BSON("x" << 100));
const KeyPattern kNestedShardKey(BSON("x.y" << 1));
const ChunkRange kNestedChunkRange(BSON("x.y" << 0), BSON("x.y" << 100));

class SessionCatalogMigrationSourceTest : public MockReplCoordServerFixture {};

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2,
                                Date_t wallClockTime,
                                LogicalSessionId sessionId,
                                TxnNumber txnNumber,
                                const std::vector<StmtId>& stmtIds,
                                repl::OpTime prevWriteOpTimeInTransaction,
                                boost::optional<repl::OpTime> preImageOpTime,
                                boost::optional<repl::OpTime> postImageOpTime,

                                boost::optional<repl::RetryImageEnum> needsRetryImage) {
    OperationSessionInfo osi;
    osi.setSessionId(sessionId);
    osi.setTxnNumber(txnNumber);

    return repl::DurableOplogEntry(
        opTime,                           // optime
        opType,                           // opType
        nss,                              // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        object,                           // o
        object2,                          // o2
        osi,                              // sessionInfo
        boost::none,                      // upsert
        wallClockTime,                    // wall clock time
        stmtIds,                          // statement ids
        prevWriteOpTimeInTransaction,     // optime of previous write within same transaction
        preImageOpTime,                   // pre-image optime
        postImageOpTime,                  // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        needsRetryImage);
}

repl::OplogEntry makeOplogEntry(
    repl::OpTime opTime,
    repl::OpTypeEnum opType,
    BSONObj object,
    boost::optional<BSONObj> object2,
    Date_t wallClockTime,
    LogicalSessionId sessionId,
    TxnNumber txnNumber,
    const std::vector<StmtId>& stmtIds,
    repl::OpTime prevWriteOpTimeInTransaction,
    boost::optional<repl::OpTime> preImageOpTime = boost::none,
    boost::optional<repl::OpTime> postImageOpTime = boost::none,

    boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none) {
    return makeOplogEntry(opTime,
                          opType,
                          kNs,
                          object,
                          object2,
                          wallClockTime,
                          sessionId,
                          txnNumber,
                          stmtIds,
                          prevWriteOpTimeInTransaction,
                          preImageOpTime,
                          postImageOpTime,
                          needsRetryImage);
}

repl::OplogEntry makeSentinelOplogEntry(const LogicalSessionId& sessionId,
                                        const TxnNumber& txnNumber,
                                        Date_t wallClockTime) {
    return makeOplogEntry({},                                        // optime
                          repl::OpTypeEnum::kNoop,                   // op type
                          {},                                        // o
                          TransactionParticipant::kDeadEndSentinel,  // o2
                          wallClockTime,                             // wall clock time
                          sessionId,
                          txnNumber,
                          {kIncompleteHistoryStmtId},  // statement id
                          repl::OpTime(Timestamp(0, 0), 0),
                          boost::none,
                          boost::none);
}

repl::OplogEntry makeRewrittenOplogInSession(repl::OpTime opTime,
                                             repl::OpTime previousWriteOpTime,
                                             BSONObj object,
                                             LogicalSessionId sessionId,
                                             TxnNumber txnNumber,
                                             int statementId) {
    auto original =
        makeOplogEntry(opTime,                     // optime
                       repl::OpTypeEnum::kInsert,  // op type
                       object,                     // o
                       boost::none,                // o2
                       Date_t::now(),              // wall clock time
                       sessionId,
                       txnNumber,
                       {statementId},         // statement ids
                       previousWriteOpTime);  // optime of previous write within same transaction

    return makeOplogEntry(original.getOpTime(),                                         // optime
                          repl::OpTypeEnum::kNoop,                                      // op type
                          BSON(SessionCatalogMigration::kSessionMigrateOplogTag << 1),  // o
                          original.getEntry().toBSON(),                                 // o2
                          original.getWallClockTime(),  // wall clock time
                          *original.getSessionId(),
                          *original.getTxnNumber(),
                          original.getStatementIds(),  // statement ids
                          original.getPrevWriteOpTimeInTransaction()
                              .value());  // optime of previous write within same transaction
};

repl::DurableReplOperation makeDurableReplOp(
    const mongo::repl::OpTypeEnum opType,
    const NamespaceString& nss,
    const BSONObj& object,
    boost::optional<BSONObj> object2,
    const std::vector<int> stmtIds,
    boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none,
    boost::optional<repl::OpTime> preImageOpTime = boost::none,
    boost::optional<repl::OpTime> postImageOpTime = boost::none) {
    auto op = repl::DurableReplOperation(opType, nss, object);
    op.setObject2(object2);
    if (stmtIds.size() == 1) {
        // This is required for making BSON equality check in the tests below work.
        op.setStatementIds({{stmtIds.front()}});
    } else if (!stmtIds.empty()) {
        op.setStatementIds({{stmtIds}});
    }
    op.setNeedsRetryImage(needsRetryImage);
    op.setPreImageOpTime(preImageOpTime);
    op.setPostImageOpTime(postImageOpTime);
    return op;
}

repl::OplogEntry makeApplyOpsOplogEntry(repl::OpTime opTime,
                                        repl::OpTime prevWriteOpTimeInTransaction,
                                        std::vector<repl::DurableReplOperation> ops,
                                        LogicalSessionId sessionId,
                                        TxnNumber txnNumber,
                                        bool isPrepare,
                                        bool isPartial) {
    BSONObjBuilder applyOpsBuilder;

    BSONArrayBuilder opsArrayBuilder = applyOpsBuilder.subarrayStart("applyOps");
    for (const auto& op : ops) {
        opsArrayBuilder.append(op.toBSON());
    }
    opsArrayBuilder.done();

    if (isPrepare) {
        applyOpsBuilder.append(repl::ApplyOpsCommandInfoBase::kPrepareFieldName, true);
    }

    if (isPartial) {
        applyOpsBuilder.append(repl::ApplyOpsCommandInfoBase::kPartialTxnFieldName, true);
    }

    repl::MutableOplogEntry op;
    op.setOpType(repl::OpTypeEnum::kCommand);
    op.setObject(applyOpsBuilder.obj());
    op.setSessionId(std::move(sessionId));
    op.setTxnNumber(std::move(txnNumber));
    op.setOpTime(opTime);
    op.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
    op.setWallClockTime(Date_t::now());
    op.setNss({});

    return {op.toBSON()};
}

repl::OplogEntry makeCommandOplogEntry(repl::OpTime opTime,
                                       repl::OpTime prevWriteOpTimeInTransaction,
                                       BSONObj commandObj,
                                       boost::optional<LogicalSessionId> sessionId,
                                       boost::optional<TxnNumber> txnNumber) {
    repl::MutableOplogEntry op;
    op.setOpType(repl::OpTypeEnum::kCommand);
    op.setObject(std::move(commandObj));
    op.setSessionId(std::move(sessionId));
    op.setTxnNumber(std::move(txnNumber));
    op.setOpTime(opTime);
    op.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
    op.setWallClockTime({});
    op.setNss({});

    return {op.toBSON()};
};


TEST_F(SessionCatalogMigrationSourceTest, NoSessionsToTransferShouldNotHaveOplog) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(0, migrationSource.untransferredCatchUpDataSize());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWrites) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time,
                       sessionId,
                       txnNumber,
                       {1},                  // statement ids
                       entry1.getOpTime());  // optime of previous write within same transaction
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry2.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWritesMultiStmtIds) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0, 1},                             // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time,
                       sessionId,
                       txnNumber,
                       {2, 3},               // statement ids
                       entry1.getOpTime());  // optime of previous write within same transaction
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry2.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWrites) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto txnNumber1 = TxnNumber{1};
    const auto sessionId2 = makeLogicalSessionIdForTest();
    const auto txnNumber2 = TxnNumber{1};

    auto entry1a = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId1,
        txnNumber1,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    auto entry1b =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time,
                       sessionId1,
                       txnNumber1,
                       {1},                   // statement ids
                       entry1a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(sessionId1);
    sessionRecord1.setTxnNum(txnNumber1);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2a = makeOplogEntry(
        repl::OpTime(Timestamp(43, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        sessionId2,
        txnNumber2,
        {3},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    auto entry2b =
        makeOplogEntry(repl::OpTime(Timestamp(789, 13), 2),  // optime
                       repl::OpTypeEnum::kDelete,            // op type
                       BSON("x" << 50),                      // o
                       boost::none,                          // o2
                       Date_t::now(),                        // wall clock time,
                       sessionId2,
                       txnNumber2,
                       {4},                   // statement ids
                       entry2a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(sessionId2);
    sessionRecord2.setTxnNum(txnNumber2);
    sessionRecord2.setLastWriteOpTime(entry2b.getOpTime());
    sessionRecord2.setLastWriteDate(entry2b.getWallClockTime());

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
            ASSERT_BSONOBJ_EQ(firstExpectedOplog.getEntry().toBSON(),
                              nextOplogResult.oplog->getEntry().toBSON());
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        }

        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            ASSERT_BSONOBJ_EQ(secondExpectedOplog.getEntry().toBSON(),
                              nextOplogResult.oplog->getEntry().toBSON());
        }
    };

    if (sessionRecord1.getSessionId().toBSON().woCompare(sessionRecord2.getSessionId().toBSON()) >
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
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        entry1.getOpTime());               // pre-image optime
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(73, 5), 2),  // optime
        repl::OpTypeEnum::kNoop,            // op type
        BSON("x" << 20),                    // o
        boost::none,                        // o2
        Date_t::now(),                      // wall clock time
        sessionId,
        txnNumber,
        {2},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry3);

    auto entry4 =
        makeOplogEntry(repl::OpTime(Timestamp(73, 6), 2),  // optime
                       repl::OpTypeEnum::kUpdate,          // op type
                       BSON("$inc" << BSON("x" << 1)),     // o
                       BSON("x" << 19),                    // o2
                       Date_t::now(),                      // wall clock time
                       sessionId,
                       txnNumber,
                       {3},                  // statement ids
                       entry2.getOpTime(),   // optime of previous write within same transaction
                       boost::none,          // pre-image optime
                       entry3.getOpTime());  // post-image optime
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry4.getOpTime());
    sessionRecord.setLastWriteDate(entry4.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry3, entry4, entry1, entry2};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       OneSessionWithFindAndModifyPreImageAndPostImageMultiStmtIds) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0, 1},                             // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {2, 3},                            // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        entry1.getOpTime());               // pre-image optime
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(73, 5), 2),  // optime
        repl::OpTypeEnum::kNoop,            // op type
        BSON("x" << 20),                    // o
        boost::none,                        // o2
        Date_t::now(),                      // wall clock time
        sessionId,
        txnNumber,
        {4, 5},                             // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry3);

    auto entry4 =
        makeOplogEntry(repl::OpTime(Timestamp(73, 6), 2),  // optime
                       repl::OpTypeEnum::kUpdate,          // op type
                       BSON("$inc" << BSON("x" << 1)),     // o
                       BSON("x" << 19),                    // o2
                       Date_t::now(),                      // wall clock time
                       sessionId,
                       txnNumber,
                       {6, 7},               // statement ids
                       entry2.getOpTime(),   // optime of previous write within same transaction
                       boost::none,          // pre-image optime
                       entry3.getOpTime());  // post-image optime
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry4.getOpTime());
    sessionRecord.setLastWriteDate(entry4.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry3, entry4, entry1, entry2};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ForgeImageEntriesWhenFetchingEntriesWithNeedsRetryImage) {
    repl::ImageEntry imageEntry;
    const auto preImage = BSON("_id" << 1 << "x" << 50);
    const auto sessionId = makeLogicalSessionIdForTest();
    const repl::OpTime imageEntryOpTime = repl::OpTime(Timestamp(52, 346), 2);
    const auto txnNumber = 1LL;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(txnNumber);
    imageEntry.setTs(imageEntryOpTime.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kConfigImagesNamespace.ns(), imageEntry.toBSON());

    // Insert an oplog entry with a non-null needsRetryImage field.
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement id
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        boost::none,                       // pre-image optime
        boost::none,                       // post-image optime
        repl::RetryImageEnum::kPreImage);  // needsRetryImage
    insertOplogEntry(entry);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    // The next oplog entry should be the forged preImage entry.
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Check that the key fields are what we expect. The destination will overwrite any unneeded
    // fields when it processes the incoming entries.
    ASSERT_BSONOBJ_EQ(preImage, nextOplogResult.oplog->getObject());
    ASSERT_EQUALS(txnNumber, nextOplogResult.oplog->getTxnNumber().value());
    ASSERT_EQUALS(sessionId, nextOplogResult.oplog->getSessionId().value());
    ASSERT_EQUALS("n", repl::OpType_serializer(nextOplogResult.oplog->getOpType()));
    ASSERT_EQ(entry.getStatementIds().size(), nextOplogResult.oplog->getStatementIds().size());
    for (size_t i = 0; i < entry.getStatementIds().size(); i++) {
        ASSERT_EQ(entry.getStatementIds()[i], nextOplogResult.oplog->getStatementIds()[i]);
    }

    // The next oplog entry should be the original entry that generated the image entry.
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
}

TEST_F(SessionCatalogMigrationSourceTest, OplogWithOtherNsShouldBeIgnored) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto txnNumber1 = TxnNumber{1};
    const auto sessionId2 = makeLogicalSessionIdForTest();
    const auto txnNumber2 = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId1,
        txnNumber1,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(sessionId1);
    sessionRecord1.setTxnNum(txnNumber1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(entry1.getWallClockTime());

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
        sessionId2,
        txnNumber2,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        boost::none,                       // pre-image optime
        boost::none,                       // post-image optime
        boost::none);                      // needsRetryImage
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(sessionId2);
    sessionRecord2.setTxnNum(txnNumber2);
    sessionRecord2.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord2.setLastWriteDate(entry2.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, SessionDumpWithMultipleNewWrites) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto txnNumber1 = TxnNumber{1};
    const auto sessionId2 = makeLogicalSessionIdForTest();
    const auto txnNumber2 = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId1,
        txnNumber1,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(sessionId1);
    sessionRecord1.setTxnNum(txnNumber1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(53, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        sessionId2,
        txnNumber2,
        {1},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(55, 12), 2),  // optime
        repl::OpTypeEnum::kInsert,           // op type
        BSON("x" << 40),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        sessionId2,
        txnNumber2,
        {2},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
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
        ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry2.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry3.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
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

TEST_F(SessionCatalogMigrationSourceTest,
       ReturnDeadEndSentinelOplogEntryForNewCommittedNonInternalTransaction) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {});
    auto applyOpsOpTime = repl::OpTime(Timestamp(110, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        false,   // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kTransaction);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
    ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), sessionId);
    ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), txnNumber);
    ASSERT_BSONOBJ_EQ(*nextOplogResult.oplog->getObject2(),
                      TransactionParticipant::kDeadEndSentinel);
    auto stmtIds = nextOplogResult.oplog->getStatementIds();
    ASSERT_EQ(stmtIds.size(), 1U);
    ASSERT_EQ(stmtIds[0], kIncompleteHistoryStmtId);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

DEATH_TEST_F(SessionCatalogMigrationSourceTest,
             ThrowUponSeeingNewCommittedForInternalTransactionForNonRetryableWrite,
             "Cannot add op time for a non-retryable internal transaction") {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    const auto sessionId = makeLogicalSessionIdWithTxnUUIDForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {});
    auto applyOpsOpTime = repl::OpTime(Timestamp(120, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        false,   // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kTransaction);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       DeriveOplogEntriesForNewCommittedInternalTransactionForRetryableWriteBasic) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    const auto txnNumber = TxnNumber{1};

    auto op1 = makeDurableReplOp(
        repl::OpTypeEnum::kUpdate, kNs, BSON("$set" << BSON("_id" << 1)), BSON("x" << 1), {1});
    // op without stmtId.
    auto op2 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 2), BSONObj(), {});
    // op for a different ns.
    auto op3 =
        makeDurableReplOp(repl::OpTypeEnum::kInsert, kOtherNs, BSON("x" << 3), BSONObj(), {3});
    auto op4 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 4), BSONObj(), {4});
    // op that does not touch the chunk being migrated.
    auto op5 =
        makeDurableReplOp(repl::OpTypeEnum::kInsert, kOtherNs, BSON("x" << -5), BSONObj(), {5});
    // WouldChangeOwningShard sentinel op.
    auto op6 = makeDurableReplOp(
        repl::OpTypeEnum::kNoop, kNs, kWouldChangeOwningShardSentinel, BSONObj(), {6});

    auto applyOpsOpTime1 = repl::OpTime(Timestamp(130, 1), 1);
    auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime1,
                                         {},  // prevOpTime
                                         {op1, op2, op3},
                                         sessionId,
                                         txnNumber,
                                         false,  // isPrepare
                                         true);  // isPartial
    insertOplogEntry(entry1);

    auto applyOpsOpTime2 = repl::OpTime(Timestamp(130, 2), 1);
    auto entry2 = makeApplyOpsOplogEntry(applyOpsOpTime2,
                                         entry1.getOpTime(),  // prevOpTime
                                         {op4, op5, op6},
                                         sessionId,
                                         txnNumber,
                                         false,   // isPrepare
                                         false);  // isPartial
    insertOplogEntry(entry2);

    auto applyOpsOpTime3 = repl::OpTime(Timestamp(130, 3), 1);
    auto entry3 = makeApplyOpsOplogEntry(applyOpsOpTime3,
                                         entry2.getOpTime(),  // prevOpTime
                                         {},
                                         sessionId,
                                         txnNumber,
                                         false,   // isPrepare
                                         false);  // isPartial
    insertOplogEntry(entry3);

    migrationSource.notifyNewWriteOpTime(
        entry3.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kTransaction);

    const auto expectedSessionId = *getParentSessionId(sessionId);
    const auto expectedTxnNumber = *sessionId.getTxnNumber();
    const std::vector<repl::DurableReplOperation> expectedOps{op6, op4, op1};

    for (const auto& op : expectedOps) {
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), expectedSessionId);
        ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), expectedTxnNumber);
        ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(), op.toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       DeriveOplogEntriesForNewCommittedInternalTransactionForRetryableWriteFetchPrePostImage) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    const auto txnNumber = TxnNumber{1};

    auto preImageOpTimeForOp2 = repl::OpTime(Timestamp(140, 1), 1);
    auto preImageEntryForOp2 = makeOplogEntry(preImageOpTimeForOp2,
                                              repl::OpTypeEnum::kNoop,
                                              BSON("x" << 2),  // o
                                              boost::none,     // o2
                                              Date_t::now(),   // wallClockTime
                                              sessionId,
                                              txnNumber,
                                              {2},  // stmtIds
                                              {});  // prevOpTime
    insertOplogEntry(preImageEntryForOp2);

    auto postImageOpTimeForOp4 = repl::OpTime(Timestamp(140, 2), 1);
    auto postImageEntryForOp4 = makeOplogEntry(postImageOpTimeForOp4,
                                               repl::OpTypeEnum::kNoop,
                                               BSON("_id" << 4 << "x" << 4),  // o
                                               boost::none,                   // o2
                                               Date_t::now(),                 // wallClockTime,
                                               sessionId,
                                               txnNumber,
                                               {4},  // stmtIds
                                               {});  // prevOpTime
    insertOplogEntry(postImageEntryForOp4);

    auto op1 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {1});
    auto op2 = makeDurableReplOp(repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("$set" << BSON("_id" << 2)),
                                 BSON("x" << 2),
                                 {1},                    // stmtIds
                                 boost::none,            // needsRetryImage
                                 preImageOpTimeForOp2);  // preImageOpTime
    auto op3 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 3), BSONObj(), {3});
    auto op4 = makeDurableReplOp(repl::OpTypeEnum::kUpdate,
                                 kNs,
                                 BSON("$set" << BSON("_id" << 4)),
                                 BSON("x" << 4),
                                 {4},
                                 boost::none,             // needsRetryImage
                                 boost::none,             // preImageOpTime
                                 postImageOpTimeForOp4);  // postImageOpTime
    auto op5 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 5), BSONObj(), {5});

    auto applyOpsOpTime1 = repl::OpTime(Timestamp(140, 3), 1);
    auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime1,
                                         {},  // prevOpTime
                                         {op1, op2, op3},
                                         sessionId,
                                         txnNumber,
                                         false,  // isPrepare
                                         true);  // isPartial
    insertOplogEntry(entry1);

    auto applyOpsOpTime2 = repl::OpTime(Timestamp(140, 4), 1);
    auto entry2 = makeApplyOpsOplogEntry(applyOpsOpTime2,
                                         entry1.getOpTime(),  // prevOpTime
                                         {op4, op5},
                                         sessionId,
                                         txnNumber,
                                         false,   // isPrepare
                                         false);  // isPartial
    insertOplogEntry(entry2);

    migrationSource.notifyNewWriteOpTime(
        entry2.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kTransaction);

    const auto expectedSessionId = *getParentSessionId(sessionId);
    const auto expectedTxnNumber = *sessionId.getTxnNumber();
    const std::vector<repl::DurableReplOperation> expectedOps{
        op5,
        postImageEntryForOp4.getDurableReplOperation(),
        op4,
        op3,
        preImageEntryForOp2.getDurableReplOperation(),
        op2,
        op1};

    for (const auto& op : expectedOps) {
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        if (nextOplogResult.oplog->getOpType() == repl::OpTypeEnum::kNoop) {
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        } else {
            ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        }
        ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), expectedSessionId);
        ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), expectedTxnNumber);
        ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(), op.toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       DeriveOplogEntriesForNewCommittedInternalTransactionForRetryableWriteForgePrePostImage) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    std::vector<repl::RetryImageEnum> cases{repl::RetryImageEnum::kPreImage,
                                            repl::RetryImageEnum::kPostImage};
    auto opTimeSecs = 150;
    for (auto imageType : cases) {
        const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        const auto txnNumber = TxnNumber{1};

        auto op1 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {1});
        auto op2 = makeDurableReplOp(repl::OpTypeEnum::kUpdate,
                                     kNs,
                                     BSON("$set" << BSON("_id" << 1)),
                                     BSON("x" << 2),
                                     {2},
                                     imageType /* needsRetryImage */);
        auto op3 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 3), BSONObj(), {3});

        auto applyOpsOpTime1 = repl::OpTime(Timestamp(opTimeSecs, 2), 1);
        auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime1,
                                             {},  // prevOpTime
                                             {op1},
                                             sessionId,
                                             txnNumber,
                                             false,  // isPrepare
                                             true);  // isPartial
        insertOplogEntry(entry1);

        auto applyOpsOpTime2 = repl::OpTime(Timestamp(opTimeSecs, 3), 1);
        auto entry2 = makeApplyOpsOplogEntry(applyOpsOpTime2,
                                             entry1.getOpTime(),  // prevOpTime
                                             {op2, op3},
                                             sessionId,
                                             txnNumber,
                                             false,   // isPrepare
                                             false);  // isPartial
        insertOplogEntry(entry2);

        repl::ImageEntry imageEntryForOp2;
        imageEntryForOp2.set_id(sessionId);
        imageEntryForOp2.setTxnNumber(txnNumber);
        imageEntryForOp2.setTs(applyOpsOpTime2.getTimestamp());
        imageEntryForOp2.setImageKind(imageType);
        imageEntryForOp2.setImage(*op2.getObject2());

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kConfigImagesNamespace.ns(), imageEntryForOp2.toBSON());

        migrationSource.notifyNewWriteOpTime(
            entry2.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kTransaction);

        const auto expectedSessionId = *getParentSessionId(sessionId);
        const auto expectedTxnNumber = *sessionId.getTxnNumber();
        const auto expectedImageOpForOp2 =
            makeDurableReplOp(repl::OpTypeEnum::kNoop,
                              kNs,
                              imageEntryForOp2.getImage(),
                              boost::none,
                              repl::variant_util::toVector<StmtId>(op2.getStatementIds()));
        const std::vector<repl::DurableReplOperation> expectedOps{
            op3, expectedImageOpForOp2, op2, op1};

        for (const auto& op : expectedOps) {
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            if (nextOplogResult.oplog->getOpType() == repl::OpTypeEnum::kNoop) {
                ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            } else {
                ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
            }
            ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), expectedSessionId);
            ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), expectedTxnNumber);
            ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(),
                              op.toBSON());
        }

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        opTimeSecs++;
    }
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldBeAbleInsertNewWritesAfterBufferWasDepleted) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    {
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(52, 345), 2),  // optime
            repl::OpTypeEnum::kInsert,            // op type
            BSON("x" << 30),                      // o
            boost::none,                          // o2
            Date_t::now(),                        // wall clock time,
            sessionId,
            txnNumber,
            {0},                                // statement ids
            repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

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
            sessionId,
            txnNumber,
            {1},                                // statement ids
            repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

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
            sessionId,
            txnNumber,
            {2},                                // statement ids
            repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }
}

TEST_F(SessionCatalogMigrationSourceTest, ReturnsDeadEndSentinelForIncompleteHistory) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                 // statement ids
        repl::OpTime(Timestamp(40, 1), 2));  // optime of previous write within same transaction
    insertOplogEntry(entry);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);

        auto oplog = *nextOplogResult.oplog;
        ASSERT_TRUE(oplog.getObject2());
        ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel, *oplog.getObject2());
        ASSERT_EQ(1, oplog.getStatementIds().size());
        ASSERT_EQ(kIncompleteHistoryStmtId, oplog.getStatementIds().front());
        ASSERT_NE(Date_t{}, oplog.getWallClockTime());

        auto sessionInfo = oplog.getOperationSessionInfo();
        ASSERT_TRUE(sessionInfo.getSessionId());
        ASSERT_EQ(sessionId, *sessionInfo.getSessionId());
        ASSERT_TRUE(sessionInfo.getTxnNumber());
        ASSERT_EQ(txnNumber, *sessionInfo.getTxnNumber());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldAssertWhenRollbackDetected) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                 // statement ids
        repl::OpTime(Timestamp(40, 1), 2));  // optime of previous write within same transaction
    insertOplogEntry(entry);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_OK(repl::ReplicationProcess::get(opCtx())->incrementRollbackID(opCtx()));

    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       ReturnDeadEndSentinelOplogEntryForCommittedNonInternalTransaction) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {});
    auto applyOpsOpTime = repl::OpTime(Timestamp(160, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        false,   // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(sessionId);
    txnRecord.setTxnNum(txnNumber);
    txnRecord.setLastWriteOpTime(repl::OpTime(entry.getOpTime()));
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

TEST_F(SessionCatalogMigrationSourceTest, IgnoreCommittedInternalTransactionForNonRetryableWrite) {
    const auto sessionId = makeLogicalSessionIdWithTxnUUIDForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {});
    auto applyOpsOpTime = repl::OpTime(Timestamp(170, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        false,   // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(sessionId);
    txnRecord.setTxnNum(txnNumber);
    txnRecord.setLastWriteOpTime(repl::OpTime(entry.getOpTime()));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kCommitted);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       DeriveOplogEntriesForCommittedInternalTransactionForRetryableWriteBasic) {
    auto opTimeSecs = 210;

    auto runTest = [&](bool isPrepared) {
        const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        const auto txnNumber = TxnNumber{1};

        auto op1 = makeDurableReplOp(
            repl::OpTypeEnum::kUpdate, kNs, BSON("$set" << BSON("_id" << 1)), BSON("x" << 1), {1});
        // op without stmtId.
        auto op2 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 2), BSONObj(), {});
        // op for a different ns.
        auto op3 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kOtherNs, BSON("x" << 3), BSONObj(), {3});
        auto op4 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 4), BSONObj(), {4});
        // op that does not touch the chunk being migrated.
        auto op5 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kOtherNs, BSON("x" << -5), BSONObj(), {5});
        // WouldChangeOwningShard sentinel op.
        auto op6 = makeDurableReplOp(
            repl::OpTypeEnum::kNoop, kNs, kWouldChangeOwningShardSentinel, BSONObj(), {6});

        auto applyOpsOpTime1 = repl::OpTime(Timestamp(opTimeSecs, 1), 1);
        auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime1,
                                             {},  // prevOpTime
                                             {op1, op2, op3},
                                             sessionId,
                                             txnNumber,
                                             false,  // isPrepare
                                             true);  // isPartial
        insertOplogEntry(entry1);

        auto applyOpsOpTime2 = repl::OpTime(Timestamp(opTimeSecs, 2), 1);
        auto entry2 = makeApplyOpsOplogEntry(applyOpsOpTime2,
                                             entry1.getOpTime(),  // prevOpTime
                                             {op4, op5, op6},
                                             sessionId,
                                             txnNumber,
                                             false,   // isPrepare
                                             false);  // isPartial
        insertOplogEntry(entry2);

        auto applyOpsOpTime3 = repl::OpTime(Timestamp(opTimeSecs, 3), 1);
        auto entry3 = makeApplyOpsOplogEntry(applyOpsOpTime3,
                                             entry2.getOpTime(),  // prevOpTime
                                             {},
                                             sessionId,
                                             txnNumber,
                                             isPrepared,  // isPrepare
                                             false);      // isPartial
        insertOplogEntry(entry3);

        repl::OpTime lastWriteOpTime;
        if (isPrepared) {
            auto commitOpTime = repl::OpTime(Timestamp(opTimeSecs, 4), 1);
            auto entry4 = makeCommandOplogEntry(commitOpTime,
                                                entry2.getOpTime(),
                                                BSON("commitTransaction" << 1),
                                                sessionId,
                                                txnNumber);
            insertOplogEntry(entry4);
            lastWriteOpTime = commitOpTime;
        } else {
            lastWriteOpTime = applyOpsOpTime3;
        }

        SessionTxnRecord txnRecord;
        txnRecord.setSessionId(sessionId);
        txnRecord.setTxnNum(txnNumber);
        txnRecord.setLastWriteOpTime(lastWriteOpTime);
        txnRecord.setLastWriteDate(Date_t::now());
        txnRecord.setState(DurableTxnStateEnum::kCommitted);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

        SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

        const auto expectedSessionId = *getParentSessionId(sessionId);
        const auto expectedTxnNumber = *sessionId.getTxnNumber();
        const std::vector<repl::DurableReplOperation> expectedOps{op6, op4, op1};

        for (const auto& op : expectedOps) {
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), expectedSessionId);
            ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), expectedTxnNumber);
            ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(),
                              op.toBSON());
        }

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

        opTimeSecs++;
        client.remove(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());
    };

    runTest(false /*isPrepared */);
    runTest(true /*isPrepared */);
}

TEST_F(SessionCatalogMigrationSourceTest,
       DeriveOplogEntriesForCommittedInternalTransactionForRetryableWriteFetchPrePostImage) {
    auto opTimeSecs = 220;

    auto runTest = [&](bool isPrepared) {
        const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        const auto txnNumber = TxnNumber{1};

        auto preImageOpTimeForOp2 = repl::OpTime(Timestamp(opTimeSecs, 1), 1);
        auto preImageEntryForOp2 = makeOplogEntry(preImageOpTimeForOp2,
                                                  repl::OpTypeEnum::kNoop,
                                                  BSON("x" << 2),  // o
                                                  boost::none,     // o2
                                                  Date_t::now(),   // wallClockTime
                                                  sessionId,
                                                  txnNumber,
                                                  {2},  // stmtIds
                                                  {});  // prevOpTime
        insertOplogEntry(preImageEntryForOp2);

        auto postImageOpTimeForOp4 = repl::OpTime(Timestamp(opTimeSecs, 2), 1);
        auto postImageEntryForOp4 = makeOplogEntry(postImageOpTimeForOp4,
                                                   repl::OpTypeEnum::kNoop,
                                                   BSON("_id" << 4 << "x" << 4),  // o
                                                   boost::none,                   // o2
                                                   Date_t::now(),                 // wallClockTime,
                                                   sessionId,
                                                   txnNumber,
                                                   {4},  // stmtIds
                                                   {});  // prevOpTime
        insertOplogEntry(postImageEntryForOp4);

        auto op1 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {1});
        auto op2 = makeDurableReplOp(repl::OpTypeEnum::kUpdate,
                                     kNs,
                                     BSON("$set" << BSON("_id" << 2)),
                                     BSON("x" << 2),
                                     {2},                    // stmtIds
                                     boost::none,            // needsRetryImage
                                     preImageOpTimeForOp2);  // preImageOpTime
        auto op3 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 3), BSONObj(), {3});
        auto op4 = makeDurableReplOp(repl::OpTypeEnum::kUpdate,
                                     kNs,
                                     BSON("$set" << BSON("_id" << 4)),
                                     BSON("x" << 4),
                                     {4},
                                     boost::none,             // needsRetryImage
                                     boost::none,             // preImageOpTime
                                     postImageOpTimeForOp4);  // postImageOpTime
        auto op5 =
            makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 5), BSONObj(), {5});

        auto applyOpsOpTime1 = repl::OpTime(Timestamp(opTimeSecs, 3), 1);
        auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime1,
                                             {},  // prevOpTime
                                             {op1, op2, op3},
                                             sessionId,
                                             txnNumber,
                                             false,  // isPrepare
                                             true);  // isPartial
        insertOplogEntry(entry1);

        auto applyOpsOpTime2 = repl::OpTime(Timestamp(opTimeSecs, 4), 1);
        auto entry2 = makeApplyOpsOplogEntry(applyOpsOpTime2,
                                             entry1.getOpTime(),  // prevOpTime
                                             {op4, op5},
                                             sessionId,
                                             txnNumber,
                                             isPrepared,  // isPrepare
                                             false);      // isPartial
        insertOplogEntry(entry2);

        repl::OpTime lastWriteOpTime;
        if (isPrepared) {
            auto commitOpTime = repl::OpTime(Timestamp(opTimeSecs, 5), 1);
            auto entry3 = makeCommandOplogEntry(commitOpTime,
                                                entry2.getOpTime(),
                                                BSON("commitTransaction" << 1),
                                                sessionId,
                                                txnNumber);
            insertOplogEntry(entry3);
            lastWriteOpTime = commitOpTime;
        } else {
            lastWriteOpTime = applyOpsOpTime2;
        }

        SessionTxnRecord txnRecord;
        txnRecord.setSessionId(sessionId);
        txnRecord.setTxnNum(txnNumber);
        txnRecord.setLastWriteOpTime(lastWriteOpTime);
        txnRecord.setLastWriteDate(Date_t::now());
        txnRecord.setState(DurableTxnStateEnum::kCommitted);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

        SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

        const auto expectedSessionId = *getParentSessionId(sessionId);
        const auto expectedTxnNumber = *sessionId.getTxnNumber();
        const std::vector<repl::DurableReplOperation> expectedOps{
            op5,
            postImageEntryForOp4.getDurableReplOperation(),
            op4,
            op3,
            preImageEntryForOp2.getDurableReplOperation(),
            op2,
            op1};

        for (const auto& op : expectedOps) {
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), expectedSessionId);
            ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), expectedTxnNumber);
            ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(),
                              op.toBSON());
        }

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

        opTimeSecs++;
        client.remove(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());
    };

    runTest(false /*isPrepared */);
    runTest(true /*isPrepared */);
}

TEST_F(SessionCatalogMigrationSourceTest,
       DeriveOplogEntriesForCommittedInternalTransactionForRetryableWriteWithLatestTxnNumber) {
    DBDirectClient client(opCtx());

    const auto parentSessionId = makeLogicalSessionIdForTest();
    auto parentTxnNumber = TxnNumber{1};

    const auto childSessionId1 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentSessionId, parentTxnNumber);
    const auto childTxnNumber1 = TxnNumber{1};

    auto op1 = makeDurableReplOp(
        repl::OpTypeEnum::kUpdate, kNs, BSON("$set" << BSON("_id" << 1)), BSON("x" << 1), {1});
    auto opTime1 = repl::OpTime(Timestamp(210, 1), 1);
    auto entry1 = makeApplyOpsOplogEntry(opTime1,
                                         {},  // prevOpTime
                                         {op1},
                                         childSessionId1,
                                         childTxnNumber1,
                                         false,   // isPrepare
                                         false);  // isPartial
    insertOplogEntry(entry1);

    SessionTxnRecord txnRecord1;
    txnRecord1.setSessionId(childSessionId1);
    txnRecord1.setTxnNum(childTxnNumber1);
    txnRecord1.setLastWriteOpTime(opTime1);
    txnRecord1.setLastWriteDate(Date_t::now());
    txnRecord1.setState(DurableTxnStateEnum::kCommitted);
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord1.toBSON());

    ++parentTxnNumber;
    const auto childSessionId2 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentSessionId, parentTxnNumber);
    const auto childTxnNumber2 = TxnNumber{1};

    auto op2 = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 2), BSONObj(), {1});
    auto opTime2 = repl::OpTime(Timestamp(210, 2), 1);
    auto entry2 = makeApplyOpsOplogEntry(opTime2,
                                         {},  // prevOpTime
                                         {op2},
                                         childSessionId2,
                                         childTxnNumber2,
                                         false,   // isPrepare
                                         false);  // isPartial
    insertOplogEntry(entry2);

    SessionTxnRecord txnRecord2;
    txnRecord2.setSessionId(childSessionId2);
    txnRecord2.setTxnNum(childTxnNumber2);
    txnRecord2.setLastWriteOpTime(opTime2);
    txnRecord2.setLastWriteDate(Date_t::now());
    txnRecord2.setState(DurableTxnStateEnum::kCommitted);
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord2.toBSON());

    {
        // Create a SessionCatalogMigrationSource. It should return only the oplog entry for the
        // internal session with the latest txnNumber.
        SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), parentSessionId);
        ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), parentTxnNumber);
        ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(), op2.toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    }

    const auto otherParentSessionId = makeLogicalSessionIdForTest();
    const auto otherParentTxnNumber = TxnNumber{1};

    auto opTime3 = repl::OpTime(Timestamp(210, 3), 1);
    auto entry3 = makeOplogEntry(opTime3,
                                 repl::OpTypeEnum::kInsert,
                                 BSON("x" << 3),  // o
                                 boost::none,     // o2
                                 Date_t::now(),   // wall clock time,
                                 otherParentSessionId,
                                 otherParentTxnNumber,
                                 {1},  // statement ids
                                 {});  // prevOpTime
    insertOplogEntry(entry3);

    SessionTxnRecord txnRecord3;
    txnRecord3.setSessionId(otherParentSessionId);
    txnRecord3.setTxnNum(otherParentTxnNumber);
    txnRecord3.setLastWriteOpTime(opTime3);
    txnRecord3.setLastWriteDate(Date_t::now());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord3.toBSON());

    {
        // Create another SessionCatalogMigrationSource. It should still return only the oplog entry
        // for the internal session with the latest txnNumber.
        SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        if (parentSessionId.toBSON().woCompare(otherParentSessionId.toBSON()) > 0) {
            ASSERT_BSONOBJ_EQ(entry3.getEntry().toBSON(),
                              nextOplogResult.oplog->getEntry().toBSON());
        } else {
            ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), parentSessionId);
            ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), parentTxnNumber);
            ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(),
                              op2.toBSON());
        }

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        nextOplogResult = migrationSource.getLastFetchedOplog();
        if (parentSessionId.toBSON().woCompare(otherParentSessionId.toBSON()) > 0) {
            ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), parentSessionId);
            ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), parentTxnNumber);
            ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(),
                              op2.toBSON());
        } else {
            ASSERT_BSONOBJ_EQ(entry3.getEntry().toBSON(),
                              nextOplogResult.oplog->getEntry().toBSON());
        }

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    }
}

TEST_F(
    SessionCatalogMigrationSourceTest,
    DeriveOplogEntriesForCommittedUnpreparedInternalTransactionForRetryableWriteForgePrePostImage) {
    auto opTimeSecs = 230;

    std::vector<repl::RetryImageEnum> cases{repl::RetryImageEnum::kPreImage,
                                            repl::RetryImageEnum::kPostImage};
    auto runTest = [&](bool isPrepared) {
        for (auto imageType : cases) {
            const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
            const auto txnNumber = TxnNumber{1};

            auto op1 =
                makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {1});
            auto op2 = makeDurableReplOp(repl::OpTypeEnum::kUpdate,
                                         kNs,
                                         BSON("$set" << BSON("_id" << 2)),
                                         BSON("x" << 2),
                                         {2},
                                         imageType /* needsRetryImage */);
            auto op3 =
                makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 3), BSONObj(), {3});

            auto applyOpsOpTime1 = repl::OpTime(Timestamp(opTimeSecs, 2), 1);
            auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime1,
                                                 {},  // prevOpTime
                                                 {op1},
                                                 sessionId,
                                                 txnNumber,
                                                 false,  // isPrepare
                                                 true);  // isPartial
            insertOplogEntry(entry1);

            auto applyOpsOpTime2 = repl::OpTime(Timestamp(opTimeSecs, 3), 1);
            auto entry2 = makeApplyOpsOplogEntry(applyOpsOpTime2,
                                                 entry1.getOpTime(),  // prevOpTime
                                                 {op2, op3},
                                                 sessionId,
                                                 txnNumber,
                                                 false,   // isPrepare
                                                 false);  // isPartial
            insertOplogEntry(entry2);

            repl::OpTime lastWriteOpTime;
            if (isPrepared) {
                auto commitOpTime = repl::OpTime(Timestamp(opTimeSecs, 4), 1);
                auto entry3 = makeCommandOplogEntry(commitOpTime,
                                                    entry2.getOpTime(),
                                                    BSON("commitTransaction" << 1),
                                                    sessionId,
                                                    txnNumber);
                insertOplogEntry(entry3);
                lastWriteOpTime = commitOpTime;
            } else {
                lastWriteOpTime = applyOpsOpTime2;
            }

            DBDirectClient client(opCtx());

            SessionTxnRecord txnRecord;
            txnRecord.setSessionId(sessionId);
            txnRecord.setTxnNum(txnNumber);
            txnRecord.setLastWriteOpTime(lastWriteOpTime);
            txnRecord.setLastWriteDate(Date_t::now());
            txnRecord.setState(DurableTxnStateEnum::kCommitted);

            client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                          txnRecord.toBSON());

            repl::ImageEntry imageEntryForOp2;
            imageEntryForOp2.set_id(sessionId);
            imageEntryForOp2.setTxnNumber(txnNumber);
            imageEntryForOp2.setTs(applyOpsOpTime2.getTimestamp());
            imageEntryForOp2.setImageKind(imageType);
            imageEntryForOp2.setImage(*op2.getObject2());

            client.insert(NamespaceString::kConfigImagesNamespace.ns(), imageEntryForOp2.toBSON());

            SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

            const auto expectedSessionId = *getParentSessionId(sessionId);
            const auto expectedTxnNumber = *sessionId.getTxnNumber();
            const auto expectedImageOpForOp2 =
                makeDurableReplOp(repl::OpTypeEnum::kNoop,
                                  kNs,
                                  imageEntryForOp2.getImage(),
                                  boost::none,
                                  repl::variant_util::toVector<StmtId>(op2.getStatementIds()));
            const std::vector<repl::DurableReplOperation> expectedOps{
                op3, expectedImageOpForOp2, op2, op1};

            for (const auto& op : expectedOps) {
                ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
                ASSERT_TRUE(migrationSource.hasMoreOplog());
                auto nextOplogResult = migrationSource.getLastFetchedOplog();
                ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
                ASSERT_EQ(*nextOplogResult.oplog->getSessionId(), expectedSessionId);
                ASSERT_EQ(*nextOplogResult.oplog->getTxnNumber(), expectedTxnNumber);
                ASSERT_BSONOBJ_EQ(nextOplogResult.oplog->getDurableReplOperation().toBSON(),
                                  op.toBSON());
            }

            ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

            opTimeSecs++;
            client.remove(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                          txnRecord.toBSON());
        }
    };

    runTest(false /*isPrepared */);
    runTest(true /*isPrepared */);
}

TEST_F(SessionCatalogMigrationSourceTest,
       ReturnDeadEndSentinelOplogEntryForPreparedNonInternalTransaction) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {});
    auto applyOpsOpTime = repl::OpTime(Timestamp(180, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        true,    // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(sessionId);
    txnRecord.setTxnNum(txnNumber);
    txnRecord.setLastWriteOpTime(repl::OpTime(entry.getOpTime()));
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

TEST_F(SessionCatalogMigrationSourceTest, IgnorePreparedInternalTransactionForNonRetryableWrite) {
    const auto sessionId = makeLogicalSessionIdWithTxnUUIDForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {});
    auto applyOpsOpTime = repl::OpTime(Timestamp(190, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        true,    // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(sessionId);
    txnRecord.setTxnNum(txnNumber);
    txnRecord.setLastWriteOpTime(repl::OpTime(entry.getOpTime()));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kPrepared);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, IgnorePreparedInternalTransactionForRetryableWrite) {
    const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    const auto txnNumber = TxnNumber{1};

    auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert, kNs, BSON("x" << 1), BSONObj(), {1});
    auto applyOpsOpTime = repl::OpTime(Timestamp(200, 1), 1);
    auto entry = makeApplyOpsOplogEntry(applyOpsOpTime,
                                        {},  // prevOpTime
                                        {op},
                                        sessionId,
                                        txnNumber,
                                        true,    // isPrepare
                                        false);  // isPartial
    insertOplogEntry(entry);

    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(sessionId);
    txnRecord.setTxnNum(txnNumber);
    txnRecord.setLastWriteOpTime(repl::OpTime(entry.getOpTime()));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kPrepared);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, IgnoreInProgressTransaction) {
    auto runTest = [&](const LogicalSessionId& sessionId) {
        const auto txnNumber = TxnNumber{1};
        SessionTxnRecord txnRecord;
        txnRecord.setSessionId(sessionId);
        txnRecord.setTxnNum(txnNumber);
        txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
        txnRecord.setLastWriteDate(Date_t::now());
        txnRecord.setState(DurableTxnStateEnum::kInProgress);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

        SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(SessionCatalogMigrationSourceTest, IgnoreAbortedTransaction) {
    auto opTimeSecs = 200;

    auto runTest = [&](const LogicalSessionId& sessionId) {
        const auto txnNumber = TxnNumber{1};

        auto applyOpsOpTime = repl::OpTime(Timestamp(opTimeSecs, 1), 1);
        auto op = makeDurableReplOp(repl::OpTypeEnum::kInsert,
                                    kNs,
                                    BSON("x" << 1),
                                    BSONObj(),
                                    isInternalSessionForRetryableWrite(sessionId)
                                        ? std::vector<StmtId>{1}
                                        : std::vector<StmtId>{});
        auto entry1 = makeApplyOpsOplogEntry(applyOpsOpTime,
                                             {},  // prevOpTime
                                             {op},
                                             sessionId,
                                             txnNumber,
                                             true,    // isPrepare
                                             false);  // isPartial
        insertOplogEntry(entry1);

        auto abortOpTime = repl::OpTime(Timestamp(opTimeSecs, 2), 1);
        auto entry2 = makeCommandOplogEntry(
            abortOpTime, applyOpsOpTime, BSON("abortTransaction" << 1), sessionId, txnNumber);
        insertOplogEntry(entry2);

        SessionTxnRecord txnRecord;
        txnRecord.setSessionId(sessionId);
        txnRecord.setTxnNum(txnNumber);
        txnRecord.setLastWriteOpTime(abortOpTime);
        txnRecord.setLastWriteDate(Date_t::now());
        txnRecord.setState(DurableTxnStateEnum::kAborted);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

        SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());

        opTimeSecs++;
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(SessionCatalogMigrationSourceTest,
       MixedTransactionEntriesAndRetryableWritesEntriesReturnCorrectResults) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto txnNumber1 = TxnNumber{1};
    const auto sessionId2 = makeLogicalSessionIdForTest();
    const auto txnNumber2 = TxnNumber{1};

    // Create an entry corresponding to a retryable write.
    auto insertOplog = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId1,
        txnNumber1,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    // Create a config.transaction entry pointing to the insert oplog entry.
    SessionTxnRecord retryableWriteRecord;
    retryableWriteRecord.setSessionId(sessionId1);
    retryableWriteRecord.setTxnNum(txnNumber1);
    retryableWriteRecord.setLastWriteOpTime(insertOplog.getOpTime());
    retryableWriteRecord.setLastWriteDate(insertOplog.getWallClockTime());

    // Create a config.transaction entry pointing to an imaginary commitTransaction entry.
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(sessionId2);
    txnRecord.setTxnNum(txnNumber2);
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
        ASSERT_BSONOBJ_EQ(insertOplog.getEntry().toBSON(),
                          nextOplogResult.oplog->getEntry().toBSON());
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
    // the sessionIds between the retryable write record and the transaction record.
    if (retryableWriteRecord.getSessionId().toBSON().woCompare(txnRecord.getSessionId().toBSON()) >
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
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        entry1.getOpTime());               // pre-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyUpdatePrePostNotTouchingChunkIsIgnored) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -5),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("y" << 1)),       // o
        BSON("x" << -5),                      // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        entry1.getOpTime());               // pre-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       UpdatePreImageTouchingPostNotTouchingChunkShouldNotBeIgnored) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("x" << -50)),     // o
        BSON("x" << 10),                      // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        boost::none,                       // pre-image optime
        entry1.getOpTime());               // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry1, entry2};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       UpdatePreImageNotTouchingPostTouchingChunkShouldBeIgnored) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("x" << 50)),      // o
        BSON("x" << -10),                     // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        boost::none,                       // pre-image optime
        entry1.getOpTime());               // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyUpdateNotTouchingChunkShouldBeIgnored) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -10 << "y" << 50),        // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("y" << 50)),      // o
        BSON("x" << -10),                     // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),  // optime of previous write within same transaction
        boost::none,                       // pre-image optime
        entry1.getOpTime());               // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWritesContainingWriteNotInChunk) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto sessionId2 = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto cmpResult = sessionId1.toBSON().woCompare(sessionId2.toBSON());
    auto lowerSessionId = (cmpResult < 0) ? sessionId1 : sessionId2;
    auto higherSessionId = (cmpResult < 0) ? sessionId2 : sessionId1;

    auto entry1a = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        lowerSessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    auto entry1b =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << -50),                       // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time,
                       lowerSessionId,
                       txnNumber,
                       {1},                   // statement ids
                       entry1a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(lowerSessionId);
    sessionRecord1.setTxnNum(txnNumber);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2a = makeOplogEntry(
        repl::OpTime(Timestamp(43, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        higherSessionId,
        txnNumber,
        {3},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    auto entry2b =
        makeOplogEntry(repl::OpTime(Timestamp(789, 13), 2),  // optime
                       repl::OpTypeEnum::kDelete,            // op type
                       BSON("x" << 50),                      // o
                       boost::none,                          // o2
                       Date_t::now(),                        // wall clock time,
                       higherSessionId,
                       txnNumber,
                       {4},                   // statement ids
                       entry2a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(higherSessionId);
    sessionRecord2.setTxnNum(txnNumber);
    sessionRecord2.setLastWriteOpTime(entry2b.getOpTime());
    sessionRecord2.setLastWriteDate(entry2b.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    insertOplogEntry(entry2a);
    insertOplogEntry(entry1a);
    insertOplogEntry(entry1b);
    insertOplogEntry(entry2b);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry1a, entry2b, entry2a};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, UntransferredDataSizeWithCommittedWrites) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    DBDirectClient client(opCtx());
    client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
    client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                         {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
    // Enter an oplog entry before creating SessionCatalogMigrationSource to set config.transactions
    // average object size to the size of this entry.
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 0),                       // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(txnNumber);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    // Check for the initial state of the SessionCatalogMigrationSource, and drain the majority
    // committed session writes.
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_FALSE(migrationSource.inCatchupPhase());
    migrationSource.fetchNextOplog(opCtx());
    migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());

    // Test inCatchupPhase() and untransferredCatchUpDataSize() with new writes.
    insertOplogEntry(entry);
    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(migrationSource.untransferredCatchUpDataSize(), sessionRecord.toBSON().objsize());

    insertOplogEntry(entry);
    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(migrationSource.untransferredCatchUpDataSize(), 2 * sessionRecord.toBSON().objsize());

    // Drain new writes and check untransferred data size.
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    ASSERT_FALSE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(0, migrationSource.untransferredCatchUpDataSize());
}

TEST_F(SessionCatalogMigrationSourceTest, UntransferredDataSizeWithNoCommittedWrites) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 0),                       // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry);
    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    // Average object size is default since the config.transactions collection does not exist.
    const int64_t defaultSessionDocSize =
        sizeof(LogicalSessionId) + sizeof(TxnNumber) + sizeof(Timestamp) + 16;
    ASSERT_EQ(migrationSource.untransferredCatchUpDataSize(), defaultSessionDocSize);
}

TEST_F(SessionCatalogMigrationSourceTest, FilterRewrittenOplogEntriesOutsideChunkRange) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto data = {std::make_pair(BSON("x" << 30), repl::OpTime(Timestamp(52, 345), 2)),
                 std::make_pair(BSON("x" << -50), repl::OpTime(Timestamp(67, 54801), 2)),
                 std::make_pair(BSON("x" << 40), repl::OpTime(Timestamp(43, 12), 2)),
                 std::make_pair(BSON("x" << 50), repl::OpTime(Timestamp(789, 13), 2))};

    std::vector<repl::OplogEntry> entries;
    std::transform(data.begin(),
                   data.end(),
                   std::back_inserter(entries),
                   [sessionId, txnNumber](const auto& pair) {
                       auto original = makeOplogEntry(
                           pair.second,                // optime
                           repl::OpTypeEnum::kInsert,  // op type
                           pair.first,                 // o
                           boost::none,                // o2
                           Date_t::now(),              // wall clock time
                           sessionId,
                           txnNumber,
                           {0},  // statement ids
                           repl::OpTime(Timestamp(0, 0),
                                        0));  // optime of previous write within same transaction
                       return makeOplogEntry(
                           pair.second,                                                  // optime
                           repl::OpTypeEnum::kNoop,                                      // op type
                           BSON(SessionCatalogMigration::kSessionMigrateOplogTag << 1),  // o
                           original.getEntry().toBSON(),                                 // o2
                           original.getWallClockTime(),  // wall clock time
                           sessionId,
                           txnNumber,
                           {0},  // statement ids
                           repl::OpTime(Timestamp(0, 0),
                                        0));  // optime of previous write within same transaction
                   });


    DBDirectClient client(opCtx());
    for (auto entry : entries) {
        SessionTxnRecord sessionRecord(
            sessionId, txnNumber, entry.getOpTime(), entry.getWallClockTime());

        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                      sessionRecord.toBSON());
        insertOplogEntry(entry);
    }
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    std::vector<repl::OplogEntry> filteredEntries = {entries.at(1)};

    while (migrationSource.fetchNextOplog(opCtx())) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        std::for_each(
            filteredEntries.begin(), filteredEntries.end(), [nextOplogResult](auto& entry) {
                ASSERT_BSONOBJ_NE(entry.getEntry().toBSON(),
                                  nextOplogResult.oplog->getEntry().toBSON());
            });
    }
}

TEST_F(SessionCatalogMigrationSourceTest,
       FilterSingleSessionRewrittenOplogEntriesOutsideChunkRange) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};

    auto rewrittenEntryOne = makeRewrittenOplogInSession(repl::OpTime(Timestamp(52, 345), 2),
                                                         repl::OpTime(Timestamp(0, 0), 0),
                                                         BSON("x" << 30),
                                                         sessionId,
                                                         txnNumber,
                                                         0);

    auto rewrittenEntryTwo = makeRewrittenOplogInSession(repl::OpTime(Timestamp(67, 54801), 2),
                                                         rewrittenEntryOne.getOpTime(),
                                                         BSON("x" << -50),
                                                         sessionId,
                                                         txnNumber,
                                                         1);

    std::vector<repl::OplogEntry> entries = {rewrittenEntryOne, rewrittenEntryTwo};

    SessionTxnRecord sessionRecord1(
        sessionId, txnNumber, rewrittenEntryTwo.getOpTime(), rewrittenEntryTwo.getWallClockTime());
    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    for (auto entry : entries) {
        insertOplogEntry(entry);
    }

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    std::vector<repl::OplogEntry> filteredEntries = {entries.at(1)};

    while (migrationSource.fetchNextOplog(opCtx())) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();

        std::for_each(
            filteredEntries.begin(), filteredEntries.end(), [nextOplogResult](auto& entry) {
                ASSERT_BSONOBJ_NE(entry.getEntry().toBSON(),
                                  nextOplogResult.oplog->getEntry().toBSON());
            });
    }
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsTrueForCrudOplogEntryOutsideChunkRange) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);

    auto skippedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << -30),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    ASSERT_TRUE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        skippedEntry, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsFalseForCrudOplogEntryInChunkRange) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);

    auto processedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        processedEntry, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsFalseForUserDocumentWithSessionMigrateOplogTag) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);

    // This oplogEntry represents the preImage document stored in a no-op oplogEntry.
    auto processedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("_id" << 5 << "x" << 30 << SessionCatalogMigration::kSessionMigrateOplogTag
                   << 1),  // o
        BSONObj(),         // o2
        Date_t::now(),     // wall clock time
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        processedEntry, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsFalseForRewrittenOplogInChunkRange) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);
    BSONObj emptyObject;

    auto rewrittenEntryOne = makeRewrittenOplogInSession(repl::OpTime(Timestamp(52, 345), 2),
                                                         repl::OpTime(Timestamp(0, 0), 0),
                                                         BSON("x" << 30),
                                                         sessionId,
                                                         txnNumber,
                                                         0);

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        rewrittenEntryOne, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsTrueForRewrittenOplogInChunkRange) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);
    BSONObj emptyObject;

    auto rewrittenEntryOne = makeRewrittenOplogInSession(repl::OpTime(Timestamp(52, 345), 2),
                                                         repl::OpTime(Timestamp(0, 0), 0),
                                                         BSON("x" << -30),
                                                         sessionId,
                                                         txnNumber,
                                                         0);

    ASSERT_TRUE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        rewrittenEntryOne, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldSkipOplogEntryReturnsFalseForDeadSentinel) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);
    auto wallClockTime = Date_t::now();
    auto deadSentinel = makeSentinelOplogEntry(makeLogicalSessionIdForTest(), 1, wallClockTime);

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        deadSentinel, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldSkipOplogEntryWorksWithNestedShardKeys) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kNestedShardKey);

    auto processedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << BSON("y" << 30)),         // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time,
        sessionId,
        txnNumber,
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        processedEntry, shardKeyPattern, kNestedChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldSkipOplogEntryWorksWithRewrittenNestedShardKeys) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNumber = TxnNumber{1};
    const auto shardKeyPattern = ShardKeyPattern(kNestedShardKey);

    auto rewrittenEntryOne = makeRewrittenOplogInSession(repl::OpTime(Timestamp(52, 345), 2),
                                                         repl::OpTime(Timestamp(0, 0), 0),
                                                         BSON("x" << BSON("y" << 30)),
                                                         sessionId,
                                                         txnNumber,
                                                         0);

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        rewrittenEntryOne, shardKeyPattern, kNestedChunkRange));
}

}  // namespace
}  // namespace mongo
