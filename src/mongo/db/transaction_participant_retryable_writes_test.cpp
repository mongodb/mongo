
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

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                BSONObj object,
                                OperationSessionInfo sessionInfo,
                                boost::optional<Date_t> wallClockTime,
                                boost::optional<StmtId> stmtId,
                                boost::optional<repl::OpTime> prevWriteOpTimeInTransaction) {
    return repl::OplogEntry(
        opTime,                        // optime
        0,                             // hash
        opType,                        // opType
        kNss,                          // namespace
        boost::none,                   // uuid
        boost::none,                   // fromMigrate
        0,                             // version
        object,                        // o
        boost::none,                   // o2
        sessionInfo,                   // sessionInfo
        boost::none,                   // upsert
        wallClockTime,                 // wall clock time
        stmtId,                        // statement id
        prevWriteOpTimeInTransaction,  // optime of previous write within same transaction
        boost::none,                   // pre-image optime
        boost::none);                  // post-image optime
}

class OpObserverMock : public OpObserverNoop {
public:
    void onTransactionPrepare(OperationContext* opCtx, const OplogSlot& prepareOpTime) override {
        ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
        OpObserverNoop::onTransactionPrepare(opCtx, prepareOpTime);

        uassert(ErrorCodes::OperationFailed,
                "onTransactionPrepare() failed",
                !onTransactionPrepareThrowsException);

        onTransactionPrepareFn();
    }

    bool onTransactionPrepareThrowsException = false;
    bool transactionPrepared = false;
    stdx::function<void()> onTransactionPrepareFn = [this]() { transactionPrepared = true; };

    void onTransactionCommit(OperationContext* opCtx,
                             boost::optional<OplogSlot> commitOplogEntryOpTime,
                             boost::optional<Timestamp> commitTimestamp) override {
        ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
        OpObserverNoop::onTransactionCommit(opCtx, commitOplogEntryOpTime, commitTimestamp);

        uassert(ErrorCodes::OperationFailed,
                "onTransactionCommit() failed",
                !onTransactionCommitThrowsException);

        onTransactionCommitFn(commitOplogEntryOpTime, commitTimestamp);
    }

    bool onTransactionCommitThrowsException = false;
    bool transactionCommitted = false;
    stdx::function<void(boost::optional<OplogSlot>, boost::optional<Timestamp>)>
        onTransactionCommitFn =
            [this](boost::optional<OplogSlot> commitOplogEntryOpTime,
                   boost::optional<Timestamp> commitTimestamp) { transactionCommitted = true; };

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  const CollectionDropType dropType) override {
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
    void setUp() final {
        MockReplCoordServerFixture::setUp();

        MongoDSessionCatalog::onStepUp(opCtx());

        const auto service = opCtx()->getServiceContext();
        OpObserverRegistry* opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        auto mockObserver = stdx::make_unique<OpObserverMock>();
        _opObserver = mockObserver.get();
        opObserverRegistry->addObserver(std::move(mockObserver));
    }

    void tearDown() final {
        _opObserver = nullptr;

        MockReplCoordServerFixture::tearDown();
    }

    static repl::OpTime logOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              UUID uuid,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              StmtId stmtId) {
        return logOp(opCtx, nss, uuid, lsid, txnNumber, stmtId, {});
    }

    static repl::OpTime logOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              UUID uuid,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              StmtId stmtId,
                              repl::OpTime prevOpTime) {
        OperationSessionInfo osi;
        osi.setSessionId(lsid);
        osi.setTxnNumber(txnNumber);

        repl::OplogLink link;
        link.prevOpTime = prevOpTime;

        return repl::logOp(opCtx,
                           "n",
                           nss,
                           uuid,
                           BSON("TestValue" << 0),
                           nullptr,
                           false,
                           Date_t::now(),
                           osi,
                           stmtId,
                           link,
                           false /* prepare */,
                           OplogSlot());
    }

    repl::OpTime writeTxnRecord(Session* session,
                                TxnNumber txnNum,
                                StmtId stmtId,
                                repl::OpTime prevOpTime,
                                boost::optional<DurableTxnStateEnum> txnState) {
        const auto uuid = UUID::gen();

        const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(session);
        txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime =
            logOp(opCtx(), kNss, uuid, session->getSessionId(), txnNum, stmtId, prevOpTime);
        txnParticipant->onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {stmtId}, opTime, Date_t::now(), txnState);
        wuow.commit();

        return opTime;
    }

    void assertTxnRecord(Session* session,
                         TxnNumber txnNum,
                         StmtId stmtId,
                         repl::OpTime opTime,
                         boost::optional<DurableTxnStateEnum> txnState) {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord = SessionTxnRecord::parse(
            IDLParserErrorContext("SessionEntryWrittenAtFirstWrite"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session->getSessionId(), txnRecord.getSessionId());
        ASSERT_EQ(txnNum, txnRecord.getTxnNum());
        ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
        ASSERT(txnRecord.getState() == txnState);
        ASSERT_EQ(txnState != boost::none,
                  txnRecordObj.hasField(SessionTxnRecord::kStateFieldName));

        const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(session);
        ASSERT_EQ(opTime, txnParticipant->getLastWriteOpTime(txnNum));

        txnParticipant->invalidate();
        txnParticipant->refreshFromStorageIfNeeded(opCtx());
        ASSERT_EQ(opTime, txnParticipant->getLastWriteOpTime(txnNum));
    }

    OpObserverMock* _opObserver = nullptr;
};

TEST_F(TransactionParticipantRetryableWritesTest, SessionEntryNotWrittenOnBegin) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    ASSERT_EQ(sessionId, session.getSessionId());
    ASSERT(txnParticipant->getLastWriteOpTime(txnNum).isNull());

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(!cursor->more());
}

TEST_F(TransactionParticipantRetryableWritesTest, SessionEntryWrittenAtFirstWrite) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 21;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    const auto opTime = writeTxnRecord(&session, txnNum, 0, {}, boost::none);

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(cursor->more());

    auto txnRecord = SessionTxnRecord::parse(
        IDLParserErrorContext("SessionEntryWrittenAtFirstWrite"), cursor->next());
    ASSERT(!cursor->more());
    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
    ASSERT(!txnRecord.getState());
    ASSERT_EQ(opTime, txnParticipant->getLastWriteOpTime(txnNum));
}

TEST_F(TransactionParticipantRetryableWritesTest,
       StartingNewerTransactionUpdatesThePersistedSession) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const auto firstOpTime = writeTxnRecord(&session, 100, 0, {}, boost::none);
    const auto secondOpTime = writeTxnRecord(&session, 200, 1, firstOpTime, boost::none);

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(cursor->more());

    auto txnRecord = SessionTxnRecord::parse(
        IDLParserErrorContext("SessionEntryWrittenAtFirstWrite"), cursor->next());
    ASSERT(!cursor->more());
    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(200, txnRecord.getTxnNum());
    ASSERT_EQ(secondOpTime, txnRecord.getLastWriteOpTime());
    ASSERT(!txnRecord.getState());
    ASSERT_EQ(secondOpTime, txnParticipant->getLastWriteOpTime(200));

    txnParticipant->invalidate();
    txnParticipant->refreshFromStorageIfNeeded(opCtx());
    ASSERT_EQ(secondOpTime, txnParticipant->getLastWriteOpTime(200));
}

TEST_F(TransactionParticipantRetryableWritesTest, TransactionTableUpdatesReplaceEntireDocument) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const auto firstOpTime = writeTxnRecord(&session, 100, 0, {}, boost::none);
    assertTxnRecord(&session, 100, 0, firstOpTime, boost::none);
    const auto secondOpTime =
        writeTxnRecord(&session, 200, 1, firstOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecord(&session, 200, 1, secondOpTime, DurableTxnStateEnum::kPrepared);
    const auto thirdOpTime =
        writeTxnRecord(&session, 300, 2, secondOpTime, DurableTxnStateEnum::kCommitted);
    assertTxnRecord(&session, 300, 2, thirdOpTime, DurableTxnStateEnum::kCommitted);
    const auto fourthOpTime = writeTxnRecord(&session, 400, 3, thirdOpTime, boost::none);
    assertTxnRecord(&session, 400, 3, fourthOpTime, boost::none);
}

TEST_F(TransactionParticipantRetryableWritesTest, StartingOldTxnShouldAssert) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    ASSERT_THROWS_CODE(txnParticipant->beginOrContinue(txnNum - 1, boost::none, boost::none),
                       AssertionException,
                       ErrorCodes::TransactionTooOld);
    ASSERT(txnParticipant->getLastWriteOpTime(txnNum).isNull());
}

TEST_F(TransactionParticipantRetryableWritesTest, SessionTransactionsCollectionNotDefaultCreated) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    // Drop the transactions table
    BSONObj dropResult;
    DBDirectClient client(opCtx());
    const auto& nss = NamespaceString::kSessionTransactionsTableNamespace;
    ASSERT(client.runCommand(nss.db().toString(), BSON("drop" << nss.coll()), dropResult));

    const TxnNumber txnNum = 21;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());

    const auto uuid = UUID::gen();
    const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);
    ASSERT_THROWS(txnParticipant->onWriteOpCompletedOnPrimary(
                      opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none),
                  AssertionException);
}

TEST_F(TransactionParticipantRetryableWritesTest, CheckStatementExecuted) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    ASSERT(!txnParticipant->checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(!txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));
    const auto firstOpTime = writeTxnRecord(&session, txnNum, 1000, {}, boost::none);
    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));

    ASSERT(!txnParticipant->checkStatementExecuted(opCtx(), txnNum, 2000));
    ASSERT(!txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));
    writeTxnRecord(&session, txnNum, 2000, firstOpTime, boost::none);
    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 2000));
    ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));

    // Invalidate the session and ensure the statements still check out
    txnParticipant->invalidate();
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 2000));

    ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));
    ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));
}

TEST_F(TransactionParticipantRetryableWritesTest, CheckStatementExecutedForOldTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    ASSERT_THROWS_CODE(txnParticipant->checkStatementExecuted(opCtx(), txnNum - 1, 0),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TransactionParticipantRetryableWritesTest,
       CheckStatementExecutedForInvalidatedTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->invalidate();

    ASSERT_THROWS_CODE(txnParticipant->checkStatementExecuted(opCtx(), 100, 0),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TransactionParticipantRetryableWritesTest,
       WriteOpCompletedOnPrimaryForOldTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    const auto uuid = UUID::gen();

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);
        txnParticipant->onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none);
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum - 1, 0);
        ASSERT_THROWS_CODE(txnParticipant->onWriteOpCompletedOnPrimary(
                               opCtx(), txnNum - 1, {0}, opTime, Date_t::now(), boost::none),
                           AssertionException,
                           ErrorCodes::ConflictingOperationInProgress);
    }
}

TEST_F(TransactionParticipantRetryableWritesTest,
       WriteOpCompletedOnPrimaryForInvalidatedTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto uuid = UUID::gen();
    const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);

    txnParticipant->invalidate();

    ASSERT_THROWS_CODE(txnParticipant->onWriteOpCompletedOnPrimary(
                           opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TransactionParticipantRetryableWritesTest,
       WriteOpCompletedOnPrimaryCommitIgnoresInvalidation) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto uuid = UUID::gen();
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);
        txnParticipant->onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none);

        txnParticipant->invalidate();

        wuow.commit();
    }

    txnParticipant->refreshFromStorageIfNeeded(opCtx());
    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 0));
}

TEST_F(TransactionParticipantRetryableWritesTest, IncompleteHistoryDueToOpLogTruncation) {
    const auto sessionId = makeLogicalSessionIdForTest();
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
                           0,                                   // statement id
                           boost::none);  // optime of previous write within same transaction

        // Intentionally skip writing the oplog entry for statement 0, so that it appears as if the
        // chain of log entries is broken because of oplog truncation

        auto entry1 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 1), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 1),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           1,                                   // statement id
                           entry0.getOpTime());  // optime of previous write within same transaction
        insertOplogEntry(entry1);

        auto entry2 =
            makeOplogEntry(repl::OpTime(Timestamp(100, 2), 0),  // optime
                           repl::OpTypeEnum::kInsert,           // op type
                           BSON("x" << 2),                      // o
                           osi,                                 // session info
                           Date_t::now(),                       // wall clock time
                           2,                                   // statement id
                           entry1.getOpTime());  // optime of previous write within same transaction
        insertOplogEntry(entry2);

        DBDirectClient client(opCtx());
        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), [&] {
            SessionTxnRecord sessionRecord;
            sessionRecord.setSessionId(sessionId);
            sessionRecord.setTxnNum(txnNum);
            sessionRecord.setLastWriteOpTime(entry2.getOpTime());
            sessionRecord.setLastWriteDate(*entry2.getWallClockTime());
            return sessionRecord.toBSON();
        }());
    }

    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    ASSERT_THROWS_CODE(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 1));
    ASSERT(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 2));

    ASSERT_THROWS_CODE(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 1));
    ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, 2));
}

TEST_F(TransactionParticipantRetryableWritesTest, ErrorOnlyWhenStmtIdBeingCheckedIsNotInCache) {
    const auto uuid = UUID::gen();
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 2;

    OperationSessionInfo osi;
    osi.setSessionId(sessionId);
    osi.setTxnNumber(txnNum);

    Session session(sessionId);
    const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(&session);
    txnParticipant->refreshFromStorageIfNeeded(opCtx());
    txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

    auto firstOpTime = ([&]() {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        const auto wallClockTime = Date_t::now();

        auto opTime = repl::logOp(opCtx(),
                                  "i",
                                  kNss,
                                  uuid,
                                  BSON("x" << 1),
                                  &TransactionParticipant::kDeadEndSentinel,
                                  false,
                                  wallClockTime,
                                  osi,
                                  1,
                                  {},
                                  false /* prepare */,
                                  OplogSlot());
        txnParticipant->onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {1}, opTime, wallClockTime, boost::none);
        wuow.commit();

        return opTime;
    })();

    {
        repl::OplogLink link;
        link.prevOpTime = firstOpTime;

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        const auto wallClockTime = Date_t::now();

        auto opTime = repl::logOp(opCtx(),
                                  "n",
                                  kNss,
                                  uuid,
                                  {},
                                  &TransactionParticipant::kDeadEndSentinel,
                                  false,
                                  wallClockTime,
                                  osi,
                                  kIncompleteHistoryStmtId,
                                  link,
                                  false /* prepare */,
                                  OplogSlot());

        txnParticipant->onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {kIncompleteHistoryStmtId}, opTime, wallClockTime, boost::none);
        wuow.commit();
    }

    {
        auto oplog = txnParticipant->checkStatementExecuted(opCtx(), txnNum, 1);
        ASSERT_TRUE(oplog);
        ASSERT_EQ(firstOpTime, oplog->getOpTime());
    }

    ASSERT_THROWS(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 2), AssertionException);

    // Should have the same behavior after loading state from storage.
    txnParticipant->invalidate();
    txnParticipant->refreshFromStorageIfNeeded(opCtx());

    {
        auto oplog = txnParticipant->checkStatementExecuted(opCtx(), txnNum, 1);
        ASSERT_TRUE(oplog);
        ASSERT_EQ(firstOpTime, oplog->getOpTime());
    }

    ASSERT_THROWS(txnParticipant->checkStatementExecuted(opCtx(), txnNum, 2), AssertionException);
}

}  // namespace
}  // namespace mongo
