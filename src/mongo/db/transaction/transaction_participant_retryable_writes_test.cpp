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

#include <boost/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                BSONObj object,
                                OperationSessionInfo sessionInfo,
                                Date_t wallClockTime,
                                const std::vector<StmtId>& stmtIds,
                                boost::optional<repl::OpTime> prevWriteOpTimeInTransaction) {
    return {repl::DurableOplogEntry(
        opTime,                        // optime
        opType,                        // opType
        kNss,                          // namespace
        boost::none,                   // uuid
        boost::none,                   // fromMigrate
        boost::none,                   // checkExistenceForDiffInsert
        0,                             // version
        object,                        // o
        boost::none,                   // o2
        sessionInfo,                   // sessionInfo
        boost::none,                   // upsert
        wallClockTime,                 // wall clock time
        stmtIds,                       // statement ids
        prevWriteOpTimeInTransaction,  // optime of previous write within same transaction
        boost::none,                   // pre-image optime
        boost::none,                   // post-image optime
        boost::none,                   // ShardId of resharding recipient
        boost::none,                   // _id
        boost::none)};                 // needsRetryImage
}

repl::OplogEntry makeApplyOpsOplogEntry(repl::OpTime opTime,
                                        repl::OpTypeEnum opType,
                                        std::vector<repl::ReplOperation> ops,
                                        OperationSessionInfo sessionInfo,
                                        Date_t wallClockTime,
                                        const std::vector<StmtId>& stmtIds,
                                        boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                                        boost::optional<repl::MultiOplogEntryType> multiOpType) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(opTime);
    oplogEntry.setOpType(opType);
    oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
    oplogEntry.setOperationSessionInfo(sessionInfo);
    oplogEntry.setWallClockTime(wallClockTime);
    oplogEntry.setStatementIds(stmtIds);
    oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
    oplogEntry.setMultiOpType(multiOpType);
    BSONObjBuilder oField;
    BSONArrayBuilder applyOpsBuilder = oField.subarrayStart("applyOps");
    for (const auto& op : ops) {
        applyOpsBuilder.append(op.toBSON());
    }
    applyOpsBuilder.doneFast();
    oplogEntry.setObject(oField.obj());
    return uassertStatusOK(repl::OplogEntry::parse(oplogEntry.toBSON()));
}

class OpObserverMock : public OpObserverNoop {
public:
    void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        OpStateAccumulator* opAccumulator = nullptr) override {
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

        uassert(ErrorCodes::OperationFailed,
                "onTransactionPrepare() failed",
                !onTransactionPrepareThrowsException);

        onTransactionPrepareFn();
    }

    bool onTransactionPrepareThrowsException = false;
    bool transactionPrepared = false;
    std::function<void()> onTransactionPrepareFn = [this]() {
        transactionPrepared = true;
    };

    void onUnpreparedTransactionCommit(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        OpStateAccumulator* opAccumulator = nullptr) override {
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

        uassert(ErrorCodes::OperationFailed,
                "onUnpreparedTransactionCommit() failed",
                !onUnpreparedTransactionCommitThrowsException);

        onUnpreparedTransactionCommitFn();
    }

    bool onUnpreparedTransactionCommitThrowsException = false;
    bool unpreparedTransactionCommitted = false;

    std::function<void()> onUnpreparedTransactionCommitFn = [this]() {
        unpreparedTransactionCommitted = true;
    };


    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept override {
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
        OpObserverNoop::onPreparedTransactionCommit(
            opCtx, commitOplogEntryOpTime, commitTimestamp, statements);

        uassert(ErrorCodes::OperationFailed,
                "onPreparedTransactionCommit() failed",
                !onPreparedTransactionCommitThrowsException);

        onPreparedTransactionCommitFn(commitOplogEntryOpTime, commitTimestamp);
    }

    bool onPreparedTransactionCommitThrowsException = false;
    bool preparedTransactionCommitted = false;
    std::function<void(OplogSlot, Timestamp)> onPreparedTransactionCommitFn =
        [this](OplogSlot commitOplogEntryOpTime, Timestamp commitTimestamp) {
            preparedTransactionCommitted = true;
        };

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  const CollectionDropType dropType,
                                  bool markFromMigrate) override {
        // If the oplog is not disabled for this namespace, then we need to reserve an op time for
        // the drop.
        if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
            OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
        }
        return {};
    }

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

class TransactionParticipantRetryableWritesTest : public MockReplCoordServerFixture {
protected:
    void setUp() {
        MockReplCoordServerFixture::setUp();
        const auto service = opCtx()->getServiceContext();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::set(
            service,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        mongoDSessionCatalog->onStepUp(opCtx());

        const auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        opObserverRegistry->addObserver(std::make_unique<OpObserverMock>());

        opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
        opContextSession.emplace(opCtx());
    }

    void tearDown() {
        opContextSession.reset();

        MockReplCoordServerFixture::tearDown();
    }

    static repl::OpTime logOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              UUID uuid,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              const std::vector<StmtId>& stmtIds) {
        return logOp(opCtx, nss, uuid, lsid, txnNumber, stmtIds, {});
    }

    static repl::OpTime logOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              UUID uuid,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              const std::vector<StmtId>& stmtIds,
                              repl::OpTime prevOpTime) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(nss);
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("TestValue" << 0));
        oplogEntry.setWallClockTime(Date_t::now());
        if (stmtIds.front() != kUninitializedStmtId) {
            oplogEntry.setSessionId(lsid);
            oplogEntry.setTxnNumber(txnNumber);
            oplogEntry.setStatementIds(stmtIds);
            oplogEntry.setPrevWriteOpTimeInTransaction(prevOpTime);
        }
        return repl::logOp(opCtx, &oplogEntry);
    }

    repl::OpTime writeTxnRecord(TxnNumber txnNum,
                                const std::vector<StmtId>& stmtIds,
                                repl::OpTime prevOpTime,
                                boost::optional<DurableTxnStateEnum> txnState) {
        opCtx()->setTxnNumber(txnNum);
        const auto session = OperationContextSession::get(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {txnNum},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);

        const auto uuid = UUID::gen();

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime =
            logOp(opCtx(), kNss, uuid, session->getSessionId(), txnNum, stmtIds, prevOpTime);

        SessionTxnRecord sessionTxnRecord;
        auto sessionId = session->getSessionId();
        sessionTxnRecord.setSessionId(sessionId);
        if (isInternalSessionForRetryableWrite(sessionId)) {
            sessionTxnRecord.setParentSessionId(*getParentSessionId(sessionId));
        }
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        sessionTxnRecord.setState(txnState);
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), stmtIds, sessionTxnRecord);
        wuow.commit();

        return opTime;
    }

    void assertTxnRecord(TxnNumber txnNum,
                         StmtId stmtId,
                         repl::OpTime opTime,
                         boost::optional<DurableTxnStateEnum> txnState) {
        const auto session = OperationContextSession::get(opCtx());

        DBDirectClient client(opCtx());
        FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
        findRequest.setFilter(BSON("_id" << session->getSessionId().toBSON()));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord = SessionTxnRecord::parse(
            IDLParserContext("SessionEntryWrittenAtFirstWrite"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session->getSessionId(), txnRecord.getSessionId());
        ASSERT_EQ(txnNum, txnRecord.getTxnNum());
        ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
        ASSERT(txnRecord.getState() == txnState);
        ASSERT_EQ(txnState != boost::none,
                  txnRecordObj.hasField(SessionTxnRecord::kStateFieldName));

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_EQ(opTime, txnParticipant.getLastWriteOpTime());

        txnParticipant.invalidate(opCtx());
        txnParticipant.refreshFromStorageIfNeeded(opCtx());
        ASSERT_EQ(opTime, txnParticipant.getLastWriteOpTime());
    }

protected:
    boost::optional<OperationContextSession> opContextSession;
};

class ShardTransactionParticipantRetryableWritesTest
    : public TransactionParticipantRetryableWritesTest {
protected:
    void setUp() {
        TransactionParticipantRetryableWritesTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    }

    void tearDown() final {
        serverGlobalParams.clusterRole = ClusterRole::None;
        TransactionParticipantRetryableWritesTest::tearDown();
    }
};

TEST_F(TransactionParticipantRetryableWritesTest, SessionEntryNotWrittenOnBegin) {
    const auto& sessionId = *opCtx()->getLogicalSessionId();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());

    DBDirectClient client(opCtx());
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON("_id" << sessionId.toBSON()));
    auto cursor = client.find(std::move(findRequest));
    ASSERT(cursor);
    ASSERT(!cursor->more());
}

TEST_F(TransactionParticipantRetryableWritesTest, SessionEntryWrittenAtFirstWrite) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto& sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 21;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    const auto opTime = writeTxnRecord(txnNum, {0}, {}, boost::none);

    DBDirectClient client(opCtx());
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON("_id" << sessionId.toBSON()));
    auto cursor = client.find(std::move(findRequest));
    ASSERT(cursor);
    ASSERT(cursor->more());

    auto txnRecord = SessionTxnRecord::parse(IDLParserContext("SessionEntryWrittenAtFirstWrite"),
                                             cursor->next());
    ASSERT(!cursor->more());
    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
    ASSERT(!txnRecord.getState());
    ASSERT_EQ(opTime, txnParticipant.getLastWriteOpTime());
}

TEST_F(TransactionParticipantRetryableWritesTest,
       StartingNewerTransactionUpdatesThePersistedSession) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto& sessionId = *opCtx()->getLogicalSessionId();

    const auto firstOpTime = writeTxnRecord(100, {0}, {}, boost::none);
    const auto secondOpTime = writeTxnRecord(200, {1}, firstOpTime, boost::none);

    DBDirectClient client(opCtx());
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON("_id" << sessionId.toBSON()));
    auto cursor = client.find(std::move(findRequest));
    ASSERT(cursor);
    ASSERT(cursor->more());

    auto txnRecord = SessionTxnRecord::parse(IDLParserContext("SessionEntryWrittenAtFirstWrite"),
                                             cursor->next());
    ASSERT(!cursor->more());
    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(200, txnRecord.getTxnNum());
    ASSERT_EQ(secondOpTime, txnRecord.getLastWriteOpTime());
    ASSERT(!txnRecord.getState());
    ASSERT_EQ(secondOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    ASSERT_EQ(secondOpTime, txnParticipant.getLastWriteOpTime());
}

TEST_F(TransactionParticipantRetryableWritesTest, TransactionTableUpdatesReplaceEntireDocument) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto firstOpTime = writeTxnRecord(100, {0}, {}, boost::none);
    assertTxnRecord(100, 0, firstOpTime, boost::none);
    const auto secondOpTime =
        writeTxnRecord(300, {2}, firstOpTime, DurableTxnStateEnum::kCommitted);
    assertTxnRecord(300, 2, secondOpTime, DurableTxnStateEnum::kCommitted);
    const auto thirdOpTime = writeTxnRecord(400, {3}, secondOpTime, boost::none);
    assertTxnRecord(400, 3, thirdOpTime, boost::none);
}

TEST_F(TransactionParticipantRetryableWritesTest,
       TransactionTableUpdatesReplaceEntireDocumentMultiStmtIds) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto firstOpTime = writeTxnRecord(100, {0, 1}, {}, boost::none);
    assertTxnRecord(100, 0, firstOpTime, boost::none);
    assertTxnRecord(100, 1, firstOpTime, boost::none);
    const auto secondOpTime =
        writeTxnRecord(300, {2, 3}, firstOpTime, DurableTxnStateEnum::kCommitted);
    assertTxnRecord(300, 2, secondOpTime, DurableTxnStateEnum::kCommitted);
    assertTxnRecord(300, 3, secondOpTime, DurableTxnStateEnum::kCommitted);
    const auto thirdOpTime = writeTxnRecord(400, {4, 5}, secondOpTime, boost::none);
    assertTxnRecord(400, 4, thirdOpTime, boost::none);
    assertTxnRecord(400, 5, thirdOpTime, boost::none);
}

TEST_F(TransactionParticipantRetryableWritesTest, StartingOldTxnShouldAssert) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {txnNum - 1},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone),
        AssertionException,
        ErrorCodes::TransactionTooOld);
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}

TEST_F(TransactionParticipantRetryableWritesTest,
       OlderRetryableWriteFailsOnSessionWithNewerRetryableWrite) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    const TxnNumber txnNum = 22;
    opCtx()->setTxnNumber(txnNum);
    const auto& sessionId = *opCtx()->getLogicalSessionId();

    StringBuilder sb;
    sb << "Retryable write with txnNumber 21 is prohibited on session " << sessionId
       << " because a newer retryable write with txnNumber 22 has already started on this session.";
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    ASSERT_THROWS_WHAT(
        txnParticipant.beginOrContinue(opCtx(),
                                       {txnNum - 1},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone),
        AssertionException,
        sb.str());
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}

TEST_F(TransactionParticipantRetryableWritesTest,
       OldTransactionFailsOnSessionWithNewerRetryableWrite) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter1(21);
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter2(22);
    opCtx()->setTxnNumber(txnNumberAndRetryCounter2.getTxnNumber());
    auto autocommit = false;
    const auto& sessionId = *opCtx()->getLogicalSessionId();

    StringBuilder sb;
    sb << "Cannot start transaction with " << txnNumberAndRetryCounter1.toBSON() << " on session "
       << sessionId
       << " because a newer retryable write with txnNumberAndRetryCounter { txnNumber: 22, "
          "txnRetryCounter: -1 } has already started on this session.";
    txnParticipant.beginOrContinue(opCtx(),
                                   txnNumberAndRetryCounter2,
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    ASSERT_THROWS_WHAT(
        txnParticipant.beginOrContinue(opCtx(),
                                       txnNumberAndRetryCounter1,
                                       autocommit,
                                       TransactionParticipant::TransactionActions::kContinue),
        AssertionException,
        sb.str());
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}

TEST_F(TransactionParticipantRetryableWritesTest, SessionTransactionsCollectionNotDefaultCreated) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto& sessionId = *opCtx()->getLogicalSessionId();

    // Drop the transactions table
    BSONObj dropResult;
    DBDirectClient client(opCtx());
    const auto& nss = NamespaceString::kSessionTransactionsTableNamespace;
    ASSERT(client.runCommand(nss.dbName(), BSON("drop" << nss.coll()), dropResult));

    const TxnNumber txnNum = 21;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());

    const auto uuid = UUID::gen();
    const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, {0});
    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setSessionId(sessionId);
    sessionTxnRecord.setTxnNum(txnNum);
    sessionTxnRecord.setLastWriteOpTime(opTime);
    sessionTxnRecord.setLastWriteDate(Date_t::now());
    ASSERT_THROWS(txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0}, sessionTxnRecord),
                  AssertionException);
}

TEST_F(TransactionParticipantRetryableWritesTest,
       CheckStatementExecutedSingleTransactionParticipant) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    ASSERT(!txnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(!txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));
    const auto firstOpTime = writeTxnRecord(txnNum, {1000}, {}, boost::none);
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));

    ASSERT(!txnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(!txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));
    writeTxnRecord(txnNum, {2000}, firstOpTime, boost::none);
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));

    // Invalidate the session and ensure the statements still check out
    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2000));

    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));
}

TEST_F(ShardTransactionParticipantRetryableWritesTest,
       CheckStatementExecutedMultipleTransactionParticipants) {
    const auto parentLsid = *opCtx()->getLogicalSessionId();
    const TxnNumber parentTxnNumber = 100;
    opCtx()->setTxnNumber(parentTxnNumber);
    const auto childLsid = makeLogicalSessionIdWithTxnNumberAndUUID(parentLsid, parentTxnNumber);
    const TxnNumber childTxnNumber = 0;

    auto parentTxnParticipant = TransactionParticipant::get(opCtx());
    parentTxnParticipant.refreshFromStorageIfNeeded(opCtx());

    parentTxnParticipant.beginOrContinue(opCtx(),
                                         {parentTxnNumber},
                                         boost::none /* autocommit */,
                                         TransactionParticipant::TransactionActions::kNone);
    ASSERT(!parentTxnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(!parentTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));
    writeTxnRecord(parentTxnNumber, {1000}, {}, boost::none);
    ASSERT(parentTxnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(parentTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));

    opContextSession.reset();
    opCtx()->setLogicalSessionId(childLsid);
    opCtx()->setTxnNumber(childTxnNumber);
    opContextSession.emplace(opCtx());

    auto childTxnParticipant = TransactionParticipant::get(opCtx());
    childTxnParticipant.refreshFromStorageIfNeeded(opCtx());

    childTxnParticipant.beginOrContinue(opCtx(),
                                        {childTxnNumber},
                                        boost::none /* autocommit */,
                                        TransactionParticipant::TransactionActions::kNone);
    ASSERT(!childTxnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(!childTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));
    writeTxnRecord(childTxnNumber, {2000}, {}, DurableTxnStateEnum::kCommitted);
    ASSERT(childTxnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(childTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));

    // The transaction history is shared across associated TransactionParticipants.
    ASSERT(parentTxnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(parentTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));
    ASSERT(childTxnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(childTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));

    // Invalidate both sessions. Verify that refreshing only the child session causes both sessions
    // to be refreshed.
    parentTxnParticipant.invalidate(opCtx());
    parentTxnParticipant.invalidate(opCtx());
    childTxnParticipant.refreshFromStorageIfNeeded(opCtx());

    ASSERT(parentTxnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(parentTxnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(parentTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));
    ASSERT(parentTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));

    ASSERT(childTxnParticipant.checkStatementExecuted(opCtx(), 1000));
    ASSERT(childTxnParticipant.checkStatementExecuted(opCtx(), 2000));
    ASSERT(childTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1000));
    ASSERT(childTxnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2000));
}

DEATH_TEST_REGEX_F(TransactionParticipantRetryableWritesTest,
                   CheckStatementExecutedForInvalidatedSelfTransactionParticipantInvariants,
                   R"#(Invariant failure.*retryableWriteTxnParticipantCatalog.isValid)#") {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.invalidate(opCtx());
    txnParticipant.checkStatementExecuted(opCtx(), 0);
}

DEATH_TEST_REGEX_F(ShardTransactionParticipantRetryableWritesTest,
                   CheckStatementExecutedForInvalidatedParentTransactionParticipantInvariants,
                   R"#(Invariant failure.*retryableWriteTxnParticipantCatalog.isValid)#") {
    const auto parentLsid = *opCtx()->getLogicalSessionId();
    const TxnNumber parentTxnNumber = 100;
    const auto childLsid = makeLogicalSessionIdWithTxnNumberAndUUID(parentLsid, parentTxnNumber);

    auto parentTxnParticipant = TransactionParticipant::get(opCtx());
    parentTxnParticipant.refreshFromStorageIfNeeded(opCtx());

    opContextSession.reset();
    opCtx()->setLogicalSessionId(childLsid);
    opContextSession.emplace(opCtx());

    auto childTxnParticipant = TransactionParticipant::get(opCtx());
    childTxnParticipant.refreshFromStorageIfNeeded(opCtx());
    opCtx()->setInMultiDocumentTransaction();
    opCtx()->setTxnNumber(0);
    childTxnParticipant.beginOrContinue(
        opCtx(), {0}, false /* autocommit */, TransactionParticipant::TransactionActions::kStart);

    parentTxnParticipant.invalidate(opCtx());
    childTxnParticipant.checkStatementExecuted(opCtx(), 0);
}

DEATH_TEST_REGEX_F(ShardTransactionParticipantRetryableWritesTest,
                   CheckStatementExecutedForInvalidatedChildTransactionParticipantInvariants,
                   R"#(Invariant failure.*retryableWriteTxnParticipantCatalog.isValid)#") {
    const auto parentLsid = *opCtx()->getLogicalSessionId();
    const TxnNumber parentTxnNumber = 100;
    const auto childLsid = makeLogicalSessionIdWithTxnNumberAndUUID(parentLsid, parentTxnNumber);

    auto parentTxnParticipant = TransactionParticipant::get(opCtx());
    parentTxnParticipant.refreshFromStorageIfNeeded(opCtx());

    opContextSession.reset();
    opCtx()->setLogicalSessionId(childLsid);
    opContextSession.emplace(opCtx());

    auto childTxnParticipant = TransactionParticipant::get(opCtx());
    childTxnParticipant.refreshFromStorageIfNeeded(opCtx());
    opCtx()->setInMultiDocumentTransaction();
    opCtx()->setTxnNumber(0);
    childTxnParticipant.beginOrContinue(
        opCtx(), {0}, false /* autocommit */, TransactionParticipant::TransactionActions::kStart);

    childTxnParticipant.invalidate(opCtx());
    parentTxnParticipant.checkStatementExecuted(opCtx(), 0);
}

DEATH_TEST_REGEX_F(
    TransactionParticipantRetryableWritesTest,
    WriteOpCompletedOnPrimaryForOldTransactionInvariants,
    R"#(Invariant failure.*sessionTxnRecord.getTxnNum\(\) == o\(\).activeTxnNumber)#") {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto& sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 100;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    const auto uuid = UUID::gen();

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, {0});

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0}, sessionTxnRecord);
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum - 1, {0});

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum - 1);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0}, sessionTxnRecord);
    }
}

DEATH_TEST_REGEX_F(
    TransactionParticipantRetryableWritesTest,
    WriteOpCompletedOnPrimaryForOldTransactionInvariantsMultiStmtIds,
    R"#(Invariant failure.*sessionTxnRecord.getTxnNum\(\) == o\(\).activeTxnNumber)#") {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto& sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 100;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    const auto uuid = UUID::gen();

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, {0, 1});

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0, 1}, sessionTxnRecord);
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum - 1, {0, 1});

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum - 1);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0, 1}, sessionTxnRecord);
    }
}

DEATH_TEST_REGEX_F(
    TransactionParticipantRetryableWritesTest,
    WriteOpCompletedOnPrimaryForInvalidatedTransactionInvariants,
    R"#(Invariant failure.*sessionTxnRecord.getTxnNum\(\) == o\(\).activeTxnNumber)#") {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto uuid = UUID::gen();
    const auto opTime = logOp(opCtx(), kNss, uuid, *opCtx()->getLogicalSessionId(), txnNum, {0});

    txnParticipant.invalidate(opCtx());

    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setSessionId(*opCtx()->getLogicalSessionId());
    sessionTxnRecord.setTxnNum(txnNum);
    sessionTxnRecord.setLastWriteOpTime(opTime);
    sessionTxnRecord.setLastWriteDate(Date_t::now());
    txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0}, sessionTxnRecord);
}

TEST_F(TransactionParticipantRetryableWritesTest, IncompleteHistoryDueToOpLogTruncation) {
    const auto sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 2;

    {
        OperationSessionInfo osi;
        osi.setSessionId(sessionId);
        osi.setTxnNumber(txnNum);

        auto entry0 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 0), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 0),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           {0},                                 // statement ids
                           boost::none);  // optime of previous write within same transaction

        // Intentionally skip writing the oplog entry for statement 0, so that it appears as if the
        // chain of log entries is broken because of oplog truncation

        auto entry1 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 1), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 1),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           {1},                                 // statement ids
                           entry0.getOpTime());  // optime of previous write within same transaction
        insertOplogEntry(entry1);

        auto entry2 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 2), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 2),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           {2},                                 // statement ids
                           entry1.getOpTime());  // optime of previous write within same transaction
        insertOplogEntry(entry2);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace, [&] {
            SessionTxnRecord sessionRecord;
            sessionRecord.setSessionId(sessionId);
            sessionRecord.setTxnNum(txnNum);
            sessionRecord.setLastWriteOpTime(entry2.getOpTime());
            sessionRecord.setLastWriteDate(entry2.getWallClockTime());
            return sessionRecord.toBSON();
        }());
    }

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    ASSERT_THROWS_CODE(txnParticipant.checkStatementExecuted(opCtx(), 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 1));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2));

    ASSERT_THROWS_CODE(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2));
}

TEST_F(TransactionParticipantRetryableWritesTest,
       IncompleteHistoryDueToOpLogTruncationMultiStmtIds) {
    const auto sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 2;

    {
        OperationSessionInfo osi;
        osi.setSessionId(sessionId);
        osi.setTxnNumber(txnNum);

        auto entry0 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 0), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 0),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           {0, 1},                              // statement ids
                           boost::none);  // optime of previous write within same transaction

        // Intentionally skip writing the oplog entry for statement 0, so that it appears as if the
        // chain of log entries is broken because of oplog truncation

        auto entry1 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 1), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 1),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           {2, 3},                              // statement ids
                           entry0.getOpTime());  // optime of previous write within same transaction
        insertOplogEntry(entry1);

        auto entry2 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 2), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 2),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           {4, 5},                              // statement ids
                           entry1.getOpTime());  // optime of previous write within same transaction
        insertOplogEntry(entry2);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace, [&] {
            SessionTxnRecord sessionRecord;
            sessionRecord.setSessionId(sessionId);
            sessionRecord.setTxnNum(txnNum);
            sessionRecord.setLastWriteOpTime(entry2.getOpTime());
            sessionRecord.setLastWriteDate(entry2.getWallClockTime());
            return sessionRecord.toBSON();
        }());
    }

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    ASSERT_THROWS_CODE(txnParticipant.checkStatementExecuted(opCtx(), 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT_THROWS_CODE(txnParticipant.checkStatementExecuted(opCtx(), 1),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 3));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 4));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 5));

    ASSERT_THROWS_CODE(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT_THROWS_CODE(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 1),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 2));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 3));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 4));
    ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx(), 5));
}

TEST_F(TransactionParticipantRetryableWritesTest, ErrorOnlyWhenStmtIdBeingCheckedIsNotInCache) {
    const auto uuid = UUID::gen();
    const auto sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 2;

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    opCtx()->setTxnNumber(txnNum);
    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setSessionId(sessionId);
    oplogEntry.setTxnNumber(txnNum);
    oplogEntry.setNss(kNss);
    oplogEntry.setUuid(uuid);

    auto firstOpTime = ([&]() {
        oplogEntry.setOpType(repl::OpTypeEnum::kInsert);
        oplogEntry.setObject(BSON("x" << 1));
        oplogEntry.setObject2(TransactionParticipant::kDeadEndSentinel);
        oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
        oplogEntry.setStatementIds({1});

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        const auto wallClockTime = Date_t::now();
        oplogEntry.setWallClockTime(wallClockTime);

        auto opTime = repl::logOp(opCtx(), &oplogEntry);

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(wallClockTime);
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {1}, sessionTxnRecord);
        wuow.commit();

        return opTime;
    })();

    {
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setObject({});
        oplogEntry.setObject2(TransactionParticipant::kDeadEndSentinel);
        oplogEntry.setPrevWriteOpTimeInTransaction(firstOpTime);
        oplogEntry.setStatementIds({kIncompleteHistoryStmtId});

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        const auto wallClockTime = Date_t::now();
        oplogEntry.setWallClockTime(wallClockTime);

        auto opTime = repl::logOp(opCtx(), &oplogEntry);

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.onWriteOpCompletedOnPrimary(
            opCtx(), {kIncompleteHistoryStmtId}, sessionTxnRecord);
        wuow.commit();
    }

    {
        auto oplog = txnParticipant.checkStatementExecuted(opCtx(), 1);
        ASSERT_TRUE(oplog);
        ASSERT_EQ(firstOpTime, oplog->getOpTime());
    }

    ASSERT_THROWS(txnParticipant.checkStatementExecuted(opCtx(), 2), AssertionException);

    // Should have the same behavior after loading state from storage.
    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    {
        auto oplog = txnParticipant.checkStatementExecuted(opCtx(), 1);
        ASSERT_TRUE(oplog);
        ASSERT_EQ(firstOpTime, oplog->getOpTime());
    }

    ASSERT_THROWS(txnParticipant.checkStatementExecuted(opCtx(), 2), AssertionException);
}

TEST_F(TransactionParticipantRetryableWritesTest, SingleRetryableApplyOps) {
    const auto sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 2;

    {
        OperationSessionInfo osi;
        osi.setSessionId(sessionId);
        osi.setTxnNumber(txnNum);
        UUID collUUID = UUID::gen();

        auto insert0 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 0 << "x" << 10), BSON("_id" << 0));
        auto insert1 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 1 << "x" << 11), BSON("_id" << 1));
        auto insert2 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 2 << "x" << 12), BSON("_id" << 2));
        insert0.setStatementIds({0});
        insert1.setStatementIds({1});
        insert2.setStatementIds({2});
        auto entry0 = makeApplyOpsOplogEntry(
            repl::OpTime(Timestamp(100, 0), 0),  // optime
            repl::OpTypeEnum::kCommand,          // op type
            {insert0, insert1, insert2},         // operations
            osi,                                 // session info
            Date_t::now(),                       // wall clock time
            {},                                  // statement ids (at top level, should be empty)
            repl::OpTime(),  // optime of previous write within same retryable write
            repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

        insertOplogEntry(entry0);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace, [&] {
            SessionTxnRecord sessionRecord;
            sessionRecord.setSessionId(sessionId);
            sessionRecord.setTxnNum(txnNum);
            sessionRecord.setLastWriteOpTime(entry0.getOpTime());
            sessionRecord.setLastWriteDate(entry0.getWallClockTime());
            return sessionRecord.toBSON();
        }());
    }

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 0));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 1));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2));
    ASSERT_FALSE(txnParticipant.checkStatementExecuted(opCtx(), 3));
}

TEST_F(TransactionParticipantRetryableWritesTest, MultipleRetryableApplyOps) {
    const auto sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 2;

    {
        OperationSessionInfo osi;
        osi.setSessionId(sessionId);
        osi.setTxnNumber(txnNum);
        UUID collUUID = UUID::gen();

        auto insert0 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 0 << "x" << 10), BSON("_id" << 0));
        auto insert1 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 1 << "x" << 11), BSON("_id" << 1));
        auto insert2 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 2 << "x" << 12), BSON("_id" << 2));
        auto insert3 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 3 << "x" << 13), BSON("_id" << 3));
        auto insert4 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 4 << "x" << 14), BSON("_id" << 4));
        auto insert5 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 5 << "x" << 15), BSON("_id" << 5));
        insert0.setStatementIds({0});
        insert1.setStatementIds({1});
        insert2.setStatementIds({2});
        insert3.setStatementIds({4});
        insert4.setStatementIds({5});
        insert5.setStatementIds({6});
        const repl::OpTime firstOpTime(Timestamp(100, 1), 1);
        const repl::OpTime lastOpTime(Timestamp(100, 2), 1);
        auto entry0 = makeApplyOpsOplogEntry(
            firstOpTime,                  // optime
            repl::OpTypeEnum::kCommand,   // op type
            {insert0, insert1, insert2},  // operations
            osi,                          // session info
            Date_t::now(),                // wall clock time
            {},                           // statement ids (at top level, should be empty)
            repl::OpTime(),               // optime of previous write within same retryable write
            repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

        auto entry1 = makeApplyOpsOplogEntry(
            lastOpTime,                   // optime
            repl::OpTypeEnum::kCommand,   // op type
            {insert3, insert4, insert5},  // operations
            osi,                          // session info
            Date_t::now(),                // wall clock time
            {},                           // statement ids (at top level, should be empty)
            firstOpTime,                  // optime of previous write within same retryable write
            repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

        insertOplogEntry(entry0);
        insertOplogEntry(entry1);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace, [&] {
            SessionTxnRecord sessionRecord;
            sessionRecord.setSessionId(sessionId);
            sessionRecord.setTxnNum(txnNum);
            sessionRecord.setLastWriteOpTime(entry1.getOpTime());
            sessionRecord.setLastWriteDate(entry1.getWallClockTime());
            return sessionRecord.toBSON();
        }());
    }

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 0));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 1));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2));
    // We skipped 3 above.
    ASSERT_FALSE(txnParticipant.checkStatementExecuted(opCtx(), 3));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 4));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 5));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 6));
    ASSERT_FALSE(txnParticipant.checkStatementExecuted(opCtx(), 7));
}

TEST_F(TransactionParticipantRetryableWritesTest, MixedInsertAndApplyOps) {
    const auto sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 2;

    {
        OperationSessionInfo osi;
        osi.setSessionId(sessionId);
        osi.setTxnNumber(txnNum);
        UUID collUUID = UUID::gen();

        auto insert0 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 0 << "x" << 10), BSON("_id" << 0));
        auto insert1 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 1 << "x" << 11), BSON("_id" << 1));
        auto insert2 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 2 << "x" << 12), BSON("_id" << 2));
        auto insert3 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 3 << "x" << 13), BSON("_id" << 3));
        auto insert4 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 4 << "x" << 14), BSON("_id" << 4));
        auto insert5 = repl::MutableOplogEntry::makeInsertOperation(
            kNss, collUUID, BSON("_id" << 5 << "x" << 15), BSON("_id" << 5));
        insert0.setStatementIds({0});
        insert1.setStatementIds({1});
        insert2.setStatementIds({2});
        insert3.setStatementIds({4});
        insert4.setStatementIds({5});
        insert5.setStatementIds({6});
        const repl::OpTime firstOpTime(Timestamp(100, 1), 1);
        const repl::OpTime secondOpTime(Timestamp(100, 2), 1);
        const repl::OpTime lastOpTime(Timestamp(100, 3), 1);
        auto entry0 = makeApplyOpsOplogEntry(
            firstOpTime,                  // optime
            repl::OpTypeEnum::kCommand,   // op type
            {insert0, insert1, insert2},  // operations
            osi,                          // session info
            Date_t::now(),                // wall clock time
            {},                           // statement ids (at top level, should be empty)
            repl::OpTime(),               // optime of previous write within same retryable write
            repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

        auto entry1 = makeApplyOpsOplogEntry(
            secondOpTime,                 // optime
            repl::OpTypeEnum::kCommand,   // op type
            {insert3, insert4, insert5},  // operations
            osi,                          // session info
            Date_t::now(),                // wall clock time
            {},                           // statement ids (at top level, should be empty)
            firstOpTime,                  // optime of previous write within same retryable write
            repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

        auto entry2 =
            makeOplogEntry(lastOpTime,                     // optime
                           repl::OpTypeEnum::kInsert,      // op type
                           BSON("_id" << 6 << "x" << 16),  // o
                           osi,                            // session info
                           Date_t::now(),                  // wall clock time
                           {7},                            // statement ids
                           secondOpTime);  // optime of previous write within same retryable write
        insertOplogEntry(entry0);
        insertOplogEntry(entry1);
        insertOplogEntry(entry2);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace, [&] {
            SessionTxnRecord sessionRecord;
            sessionRecord.setSessionId(sessionId);
            sessionRecord.setTxnNum(txnNum);
            sessionRecord.setLastWriteOpTime(entry2.getOpTime());
            sessionRecord.setLastWriteDate(entry2.getWallClockTime());
            return sessionRecord.toBSON();
        }());
    }

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 0));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 1));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 2));
    // We skipped 3 above.
    ASSERT_FALSE(txnParticipant.checkStatementExecuted(opCtx(), 3));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 4));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 5));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 6));
    ASSERT(txnParticipant.checkStatementExecuted(opCtx(), 7));
    ASSERT_FALSE(txnParticipant.checkStatementExecuted(opCtx(), 8));
}

TEST_F(TransactionParticipantRetryableWritesTest, RefreshFromStorageAsSecondary) {
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));
    TransactionParticipant::get(opCtx()).refreshFromStorageIfNeeded(opCtx());
}

/**
 * Test fixture for a transaction participant running on a shard server.
 */
class ShardTxnParticipantRetryableWritesTest : public TransactionParticipantRetryableWritesTest {
protected:
    void setUp() final {
        TransactionParticipantRetryableWritesTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    }

    void tearDown() final {
        serverGlobalParams.clusterRole = ClusterRole::None;
        TransactionParticipantRetryableWritesTest::tearDown();
    }
};

TEST_F(ShardTxnParticipantRetryableWritesTest,
       RestartingTxnWithExecutedRetryableWriteShouldAssert) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    const auto& sessionId = *opCtx()->getLogicalSessionId();
    const TxnNumber txnNum = 20;
    opCtx()->setTxnNumber(txnNum);
    const auto uuid = UUID::gen();

    txnParticipant.beginOrContinue(opCtx(),
                                   {txnNum},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, {0});

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(sessionId);
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {0}, sessionTxnRecord);
        wuow.commit();
    }

    auto autocommit = false;
    auto startTransaction = TransactionParticipant::TransactionActions::kStart;
    opCtx()->setInMultiDocumentTransaction();

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), {txnNum}, autocommit, startTransaction),
        AssertionException,
        50911);

    // Should have the same behavior after loading state from storage.
    txnParticipant.invalidate(opCtx());
    txnParticipant.refreshFromStorageIfNeeded(opCtx());

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(), {txnNum}, autocommit, startTransaction),
        AssertionException,
        50911);
}

}  // namespace
}  // namespace mongo
