/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/optional/optional_io.hpp>
#include <vector>

#include "mongo/db/catalog_raii.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/session_update_tracker.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ReshardingOplogSessionApplicationTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        {
            auto opCtx = makeOperationContext();
            auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
            ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
            repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());

            auto storageImpl = std::make_unique<repl::StorageInterfaceImpl>();
            repl::StorageInterface::set(serviceContext, std::move(storageImpl));

            MongoDSessionCatalog::onStepUp(opCtx.get());
        }
    }

    repl::OpTime insertSessionRecord(OperationContext* opCtx,
                                     LogicalSessionId lsid,
                                     TxnNumber txnNumber,
                                     std::vector<StmtId> stmtIds) {
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);

        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx, txnNumber, boost::none, boost::none);

        WriteUnitOfWork wuow(opCtx);
        auto opTime = repl::getNextOpTime(opCtx);
        SessionTxnRecord sessionTxnRecord(*opCtx->getLogicalSessionId(),
                                          *opCtx->getTxnNumber(),
                                          opTime,
                                          opCtx->getServiceContext()->getFastClockSource()->now());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx, std::move(stmtIds), sessionTxnRecord);
        wuow.commit();

        return opTime;
    }

    repl::OpTime makePreparedTxn(OperationContext* opCtx,
                                 LogicalSessionId lsid,
                                 TxnNumber txnNumber) {
        opCtx->setInMultiDocumentTransaction();
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);

        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(
            opCtx, txnNumber, false /* autocommit */, true /* startTransaction */);

        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");

        // The transaction machinery cannot store an empty locker.
        { Lock::GlobalLock globalLock(opCtx, MODE_IX); }
        auto opTime = repl::getNextOpTime(opCtx);
        txnParticipant.prepareTransaction(opCtx, opTime);
        txnParticipant.stashTransactionResources(opCtx);

        return opTime;
    }

    void clearPreparedTxn(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber) {
        opCtx->setInMultiDocumentTransaction();
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);

        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(
            opCtx, txnNumber, false /* autocommit */, boost::none /* startTransaction */);

        txnParticipant.unstashTransactionResources(opCtx, "abortTransaction");
        txnParticipant.abortTransaction(opCtx);
        txnParticipant.stashTransactionResources(opCtx);
    }

    repl::OplogEntry makeUpdateOp(
        BSONObj document,
        LogicalSessionId lsid,
        TxnNumber txnNumber,
        const std::vector<StmtId>& stmtIds,
        boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kUpdate);
        op.setObject2(document["_id"].wrap().getOwned());
        op.setObject(std::move(document));
        op.setSessionId(std::move(lsid));
        op.setTxnNumber(std::move(txnNumber));
        op.setStatementIds(stmtIds);
        op.setNeedsRetryImage(needsRetryImage);

        // These are unused by ReshardingOplogSessionApplication but required by IDL parsing.
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    repl::OplogEntry makeFinishTxnOp(LogicalSessionId lsid, TxnNumber txnNumber) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kCommand);
        // Use AbortTransactionOplogObject rather than CommitTransactionOplogObject to avoid needing
        // to deal with the commitTimestamp value. Both are treated the same way by
        // ReshardingOplogSessionApplication anyway.
        op.setObject(AbortTransactionOplogObject{}.toBSON());
        op.setSessionId(std::move(lsid));
        op.setTxnNumber(std::move(txnNumber));

        // These are unused by ReshardingOplogSessionApplication but required by IDL parsing.
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    std::vector<repl::DurableOplogEntry> findOplogEntriesNewerThan(OperationContext* opCtx,
                                                                   Timestamp ts) {
        std::vector<repl::DurableOplogEntry> result;

        PersistentTaskStore<repl::OplogEntryBase> store(NamespaceString::kRsOplogNamespace);
        store.forEach(opCtx, QUERY("ts" << BSON("$gt" << ts)), [&](const auto& oplogEntry) {
            result.emplace_back(
                unittest::assertGet(repl::DurableOplogEntry::parse(oplogEntry.toBSON())));
            return true;
        });

        return result;
    }

    boost::optional<SessionTxnRecord> findSessionRecord(OperationContext* opCtx,
                                                        const LogicalSessionId& lsid) {
        boost::optional<SessionTxnRecord> result;

        PersistentTaskStore<SessionTxnRecord> store(
            NamespaceString::kSessionTransactionsTableNamespace);
        store.forEach(opCtx,
                      QUERY(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON()),
                      [&](const auto& sessionTxnRecord) {
                          result.emplace(sessionTxnRecord);
                          return false;
                      });

        return result;
    }

    void checkGeneratedNoop(const repl::DurableOplogEntry& foundOp,
                            const LogicalSessionId& lsid,
                            TxnNumber txnNumber,
                            const std::vector<StmtId>& stmtIds) {
        ASSERT_EQ(OpType_serializer(foundOp.getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kNoop))
            << foundOp;

        ASSERT_EQ(foundOp.getSessionId(), lsid) << foundOp;
        ASSERT_EQ(foundOp.getTxnNumber(), txnNumber) << foundOp;
        ASSERT(foundOp.getStatementIds() == stmtIds) << foundOp;

        // The oplog entry must have o2 and fromMigrate set or SessionUpdateTracker will ignore it.
        ASSERT_TRUE(foundOp.getObject2());
        ASSERT_TRUE(foundOp.getFromMigrate());
    }

    void checkSessionTxnRecord(const SessionTxnRecord& sessionTxnRecord,
                               const repl::DurableOplogEntry& foundOp) {
        ASSERT_EQ(sessionTxnRecord.getSessionId(), foundOp.getSessionId())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
        ASSERT_EQ(sessionTxnRecord.getTxnNum(), foundOp.getTxnNumber())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
        ASSERT_EQ(sessionTxnRecord.getLastWriteOpTime(), foundOp.getOpTime())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
        ASSERT_EQ(sessionTxnRecord.getLastWriteDate(), foundOp.getWallClockTime())
            << sessionTxnRecord.toBSON() << ", " << foundOp;

        // Verify secondaries during replication's oplog application would generate an identical
        // SessionTxnRecord update from the oplog entry.
        repl::SessionUpdateTracker sessionUpdateTracker;
        auto flushImmediate = sessionUpdateTracker.updateSession({foundOp.toBSON()});
        auto flushLater = sessionUpdateTracker.flushAll();

        // The oplog entries generated by ReshardingTxnCloner are always op='n' and are therefore
        // buffered by SessionUpdateTracker rather than flushed immediately.
        ASSERT_FALSE(bool(flushImmediate));
        ASSERT_EQ(flushLater.size(), 1U);

        ASSERT_EQ(OpType_serializer(flushLater[0].getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kUpdate))
            << flushLater[0].getEntry();
        ASSERT_BSONOBJ_BINARY_EQ(*flushLater[0].getObject2(),
                                 BSON("_id" << sessionTxnRecord.getSessionId().toBSON()));
        ASSERT_BSONOBJ_BINARY_EQ(flushLater[0].getObject(), sessionTxnRecord.toBSON());
    }

    void checkStatementExecuted(OperationContext* opCtx,
                                LogicalSessionId lsid,
                                TxnNumber txnNumber,
                                StmtId stmtId) {
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);

        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx, txnNumber, boost::none, boost::none);
        ASSERT_TRUE(bool(txnParticipant.checkStatementExecuted(opCtx, stmtId)));
    }
};

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteForNewSession) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber incomingTxnNumber = 100;
    StmtId incomingStmtId = 2;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), makeLogicalSessionIdForTest(), 100, {3});
    }();

    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, incomingTxnNumber, {incomingStmtId});

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteForNewSessionMultiStmtIds) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber incomingTxnNumber = 100;
    StmtId incomingStmtId = 1;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), makeLogicalSessionIdForTest(), 100, {3, 4});
    }();

    auto oplogEntry = makeUpdateOp(
        BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId, incomingStmtId + 1});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(
            foundOps[0], lsid, incomingTxnNumber, {incomingStmtId, incomingStmtId + 1});

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId);
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId + 1);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteHasHigherTxnNumber) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;
    StmtId existingStmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, existingTxnNumber, {existingStmtId});
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    StmtId incomingStmtId = 2;
    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, incomingTxnNumber, {incomingStmtId});

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId);
        ASSERT_THROWS_CODE(
            checkStatementExecuted(opCtx.get(), lsid, existingTxnNumber, existingStmtId),
            DBException,
            ErrorCodes::TransactionTooOld);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteHasLowerTxnNumber) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;
    StmtId existingStmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, existingTxnNumber, {existingStmtId});
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber - 1;
    StmtId incomingStmtId = 2;
    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        ASSERT_EQ(sessionTxnRecord->getSessionId(), lsid) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getTxnNum(), existingTxnNumber) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getLastWriteOpTime(), opTime) << sessionTxnRecord->toBSON();
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_THROWS_CODE(
            checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId),
            DBException,
            ErrorCodes::TransactionTooOld);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest,
       IncomingRetryableWriteHasEqualTxnNumberAsRetryableWrite) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber txnNumber = 100;
    StmtId existingStmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, txnNumber, {existingStmtId});
    }();

    StmtId incomingStmtId = 2;
    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, txnNumber, {incomingStmtId});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, txnNumber, {incomingStmtId});
        ASSERT_EQ(foundOps[0].getPrevWriteOpTimeInTransaction(), opTime) << foundOps[0];

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, txnNumber, incomingStmtId);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteHasEqualTxnNumberAsTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber txnNumber = 100;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        auto opTime = makePreparedTxn(opCtx.get(), lsid, txnNumber);
        clearPreparedTxn(opCtx.get(), lsid, txnNumber);
        return opTime;
    }();

    StmtId incomingStmtId = 2;
    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, txnNumber, {incomingStmtId});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_FALSE(bool(sessionTxnRecord));
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteStmtAlreadyExecuted) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber txnNumber = 100;
    StmtId stmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, txnNumber, {stmtId});
    }();

    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, txnNumber, {stmtId});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        ASSERT_EQ(sessionTxnRecord->getSessionId(), lsid) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getTxnNum(), txnNumber) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getLastWriteOpTime(), opTime) << sessionTxnRecord->toBSON();
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteStmtsAlreadyExecuted) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber txnNumber = 100;
    StmtId stmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, txnNumber, {stmtId, stmtId + 1});
    }();

    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, txnNumber, {stmtId, stmtId + 1});

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        ASSERT_EQ(sessionTxnRecord->getSessionId(), lsid) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getTxnNum(), txnNumber) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getLastWriteOpTime(), opTime) << sessionTxnRecord->toBSON();
    }
}

TEST_F(ReshardingOplogSessionApplicationTest,
       IncomingRetryableWriteHasHigherTxnNumberThanPreparedTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return makePreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    StmtId incomingStmtId = 2;
    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId});

    auto hitPreparedTxn = [&] {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        return applier.tryApplyOperation(opCtx.get(), oplogEntry);
    }();

    ASSERT_TRUE(bool(hitPreparedTxn));
    ASSERT_FALSE(hitPreparedTxn->isReady());

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_FALSE(bool(sessionTxnRecord));
    }

    {
        auto opCtx = makeOperationContext();
        clearPreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }

    ASSERT_TRUE(hitPreparedTxn->isReady());
    ASSERT_OK(hitPreparedTxn->getNoThrow());
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteHasPreImage) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber incomingTxnNumber = 100;
    StmtId incomingStmtId = 2;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), makeLogicalSessionIdForTest(), 100, {3});
    }();

    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId});

    repl::MutableOplogEntry noopEntry =
        unittest::assertGet(repl::MutableOplogEntry::parse(oplogEntry.getEntry().toBSON()));
    noopEntry.setOpType(repl::OpTypeEnum::kNoop);
    noopEntry.setObject(BSON("_id" << 1 << "preImage" << true));
    oplogEntry.setPreImageOp(noopEntry.toBSON());

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 2U);
        ASSERT_BSONOBJ_BINARY_EQ(foundOps[0].getObject(), BSON("_id" << 1 << "preImage" << true));
        checkGeneratedNoop(foundOps[1], lsid, incomingTxnNumber, {incomingStmtId});
        ASSERT_EQ(foundOps[1].getPreImageOpTime(), foundOps[0].getOpTime()) << foundOps[1];

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[1]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteHasPostImage) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber incomingTxnNumber = 100;
    StmtId incomingStmtId = 2;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), makeLogicalSessionIdForTest(), 100, {3});
    }();

    auto oplogEntry = makeUpdateOp(BSON("_id" << 1), lsid, incomingTxnNumber, {incomingStmtId});

    repl::MutableOplogEntry noopEntry =
        unittest::assertGet(repl::MutableOplogEntry::parse(oplogEntry.getEntry().toBSON()));
    noopEntry.setOpType(repl::OpTypeEnum::kNoop);
    noopEntry.setObject(BSON("_id" << 1 << "postImage" << true));
    oplogEntry.setPostImageOp(noopEntry.toBSON());

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 2U);
        ASSERT_BSONOBJ_BINARY_EQ(foundOps[0].getObject(), BSON("_id" << 1 << "postImage" << true));
        checkGeneratedNoop(foundOps[1], lsid, incomingTxnNumber, {incomingStmtId});
        ASSERT_EQ(foundOps[1].getPostImageOpTime(), foundOps[0].getOpTime()) << foundOps[1];

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[1]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId);
    }
}

// Resharding converts oplog with retry image to old style no-op oplog pairs in normal cases. But
// if it was not able to extract the document from the image collection, it will return the oplog
// entry as is. This is how resharding oplog application can encounter oplog with needsRetryImage.
TEST_F(ReshardingOplogSessionApplicationTest, IncomingRetryableWriteHasNeedsRetryImage) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber incomingTxnNumber = 100;
    StmtId incomingStmtId = 2;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), makeLogicalSessionIdForTest(), 100, {3});
    }();

    auto oplogEntry = makeUpdateOp(BSON("_id" << 1),
                                   lsid,
                                   incomingTxnNumber,
                                   {incomingStmtId},
                                   repl::RetryImageEnum::kPreImage);

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, incomingTxnNumber, {incomingStmtId});
        ASSERT_FALSE(foundOps[0].getPostImageOpTime());
        ASSERT_FALSE(foundOps[0].getPreImageOpTime());
        ASSERT_FALSE(foundOps[0].getNeedsRetryImage());

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }

    {
        auto opCtx = makeOperationContext();
        checkStatementExecuted(opCtx.get(), lsid, incomingTxnNumber, incomingStmtId);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingTxnForNewSession) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber incomingTxnNumber = 100;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), makeLogicalSessionIdForTest(), 100, {3});
    }();

    auto oplogEntry = makeFinishTxnOp(lsid, incomingTxnNumber);

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, incomingTxnNumber, {kIncompleteHistoryStmtId});

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingTxnHasHigherTxnNumber) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;
    StmtId existingStmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, existingTxnNumber, {existingStmtId});
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    auto oplogEntry = makeFinishTxnOp(lsid, incomingTxnNumber);

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, incomingTxnNumber, {kIncompleteHistoryStmtId});

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingTxnHasLowerTxnNumber) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;
    StmtId existingStmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, existingTxnNumber, {existingStmtId});
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber - 1;
    auto oplogEntry = makeFinishTxnOp(lsid, incomingTxnNumber);

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        ASSERT_EQ(sessionTxnRecord->getSessionId(), lsid) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getTxnNum(), existingTxnNumber) << sessionTxnRecord->toBSON();
        ASSERT_EQ(sessionTxnRecord->getLastWriteOpTime(), opTime) << sessionTxnRecord->toBSON();
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingTxnHasEqualTxnNumberAsRetryableWrite) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber txnNumber = 100;
    StmtId existingStmtId = 3;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return insertSessionRecord(opCtx.get(), lsid, txnNumber, {existingStmtId});
    }();

    auto oplogEntry = makeFinishTxnOp(lsid, txnNumber);

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 1U);
        checkGeneratedNoop(foundOps[0], lsid, txnNumber, {kIncompleteHistoryStmtId});
        ASSERT_EQ(foundOps[0].getPrevWriteOpTimeInTransaction(), opTime) << foundOps[0];

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingTxnHasEqualTxnNumberAsTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber txnNumber = 100;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        auto opTime = makePreparedTxn(opCtx.get(), lsid, txnNumber);
        clearPreparedTxn(opCtx.get(), lsid, txnNumber);
        return opTime;
    }();

    auto oplogEntry = makeFinishTxnOp(lsid, txnNumber);

    {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        auto hitPreparedTxn = applier.tryApplyOperation(opCtx.get(), oplogEntry);
        ASSERT_FALSE(bool(hitPreparedTxn));
    }

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_FALSE(bool(sessionTxnRecord));
    }
}

TEST_F(ReshardingOplogSessionApplicationTest, IncomingTxnHasHigherTxnNumberThanPreparedTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return makePreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    auto oplogEntry = makeFinishTxnOp(lsid, incomingTxnNumber);

    auto hitPreparedTxn = [&] {
        auto opCtx = makeOperationContext();
        ReshardingOplogSessionApplication applier;
        return applier.tryApplyOperation(opCtx.get(), oplogEntry);
    }();

    ASSERT_TRUE(bool(hitPreparedTxn));
    ASSERT_FALSE(hitPreparedTxn->isReady());

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 0U);

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_FALSE(bool(sessionTxnRecord));
    }

    {
        auto opCtx = makeOperationContext();
        clearPreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }

    ASSERT_TRUE(hitPreparedTxn->isReady());
    ASSERT_OK(hitPreparedTxn->getNoThrow());
}

}  // namespace
}  // namespace mongo
