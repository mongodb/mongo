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
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
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
    void onTransactionPrepare(OperationContext* opCtx, const OplogSlot& prepareOpTime) override;
    bool onTransactionPrepareThrowsException = false;
    bool transactionPrepared = false;
    stdx::function<void()> onTransactionPrepareFn = [this]() { transactionPrepared = true; };

    void onTransactionCommit(OperationContext* opCtx, bool wasPrepared) override;
    bool onTransactionCommitThrowsException = false;
    bool transactionCommitted = false;
    stdx::function<void(bool)> onTransactionCommitFn = [this](bool wasPrepared) {
        transactionCommitted = true;
    };
};

void OpObserverMock::onTransactionPrepare(OperationContext* opCtx, const OplogSlot& prepareOpTime) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onTransactionPrepare(opCtx, prepareOpTime);

    uassert(ErrorCodes::OperationFailed,
            "onTransactionPrepare() failed",
            !onTransactionPrepareThrowsException);

    onTransactionPrepareFn();
}

void OpObserverMock::onTransactionCommit(OperationContext* opCtx, bool wasPrepared) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onTransactionCommit(opCtx, wasPrepared);

    uassert(ErrorCodes::OperationFailed,
            "onTransactionCommit() failed",
            !onTransactionCommitThrowsException);

    onTransactionCommitFn(wasPrepared);
}

class SessionTest : public MockReplCoordServerFixture {
protected:
    void setUp() final {
        MockReplCoordServerFixture::setUp();

        auto service = opCtx()->getServiceContext();
        SessionCatalog::get(service)->reset_forTest();
        SessionCatalog::get(service)->onStepUp(opCtx());

        OpObserverRegistry* opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        auto mockObserver = stdx::make_unique<OpObserverMock>();
        _opObserver = mockObserver.get();
        opObserverRegistry->addObserver(std::move(mockObserver));
    }

    void tearDown() final {
        MockReplCoordServerFixture::tearDown();
        _opObserver = nullptr;
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
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
        session->beginOrContinueTxn(opCtx(), txnNum);

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime =
            logOp(opCtx(), kNss, uuid, session->getSessionId(), txnNum, stmtId, prevOpTime);
        session->onWriteOpCompletedOnPrimary(
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
        ASSERT_EQ(opTime, session->getLastWriteOpTime(txnNum));

        session->invalidate();
        session->refreshFromStorageIfNeeded(opCtx());
        ASSERT_EQ(opTime, session->getLastWriteOpTime(txnNum));
    }

    OpObserverMock* _opObserver = nullptr;
};

TEST_F(SessionTest, SessionEntryNotWrittenOnBegin) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    session.beginOrContinueTxn(opCtx(), txnNum);

    ASSERT_EQ(sessionId, session.getSessionId());
    ASSERT(session.getLastWriteOpTime(txnNum).isNull());

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(!cursor->more());
}

TEST_F(SessionTest, SessionEntryWrittenAtFirstWrite) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 21;
    session.beginOrContinueTxn(opCtx(), txnNum);

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
    ASSERT_EQ(opTime, session.getLastWriteOpTime(txnNum));
}

TEST_F(SessionTest, StartingNewerTransactionUpdatesThePersistedSession) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

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
    ASSERT_EQ(secondOpTime, session.getLastWriteOpTime(200));

    session.invalidate();
    session.refreshFromStorageIfNeeded(opCtx());
    ASSERT_EQ(secondOpTime, session.getLastWriteOpTime(200));
}

TEST_F(SessionTest, TransactionTableUpdatesReplaceEntireDocument) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

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

TEST_F(SessionTest, StartingOldTxnShouldAssert) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    session.beginOrContinueTxn(opCtx(), txnNum);

    ASSERT_THROWS_CODE(session.beginOrContinueTxn(opCtx(), txnNum - 1),
                       AssertionException,
                       ErrorCodes::TransactionTooOld);
    ASSERT(session.getLastWriteOpTime(txnNum).isNull());
}

TEST_F(SessionTest, SessionTransactionsCollectionNotDefaultCreated) {
    const auto uuid = UUID::gen();
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    // Drop the transactions table
    BSONObj dropResult;
    DBDirectClient client(opCtx());
    const auto& nss = NamespaceString::kSessionTransactionsTableNamespace;
    ASSERT(client.runCommand(nss.db().toString(), BSON("drop" << nss.coll()), dropResult));

    const TxnNumber txnNum = 21;
    session.beginOrContinueTxn(opCtx(), txnNum);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);
    ASSERT_THROWS(session.onWriteOpCompletedOnPrimary(
                      opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none),
                  AssertionException);
}

TEST_F(SessionTest, CheckStatementExecuted) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum);

    ASSERT(!session.checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(!session.checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));
    const auto firstOpTime = writeTxnRecord(&session, txnNum, 1000, {}, boost::none);
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));

    ASSERT(!session.checkStatementExecuted(opCtx(), txnNum, 2000));
    ASSERT(!session.checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));
    writeTxnRecord(&session, txnNum, 2000, firstOpTime, boost::none);
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 2000));
    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));

    // Invalidate the session and ensure the statements still check out
    session.invalidate();
    session.refreshFromStorageIfNeeded(opCtx());

    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 2000));

    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));
    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));
}

TEST_F(SessionTest, CheckStatementExecutedForOldTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum);

    ASSERT_THROWS_CODE(session.checkStatementExecuted(opCtx(), txnNum - 1, 0),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, CheckStatementExecutedForInvalidatedTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.invalidate();

    ASSERT_THROWS_CODE(session.checkStatementExecuted(opCtx(), 100, 0),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryForOldTransactionThrows) {
    const auto uuid = UUID::gen();
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum);

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none);
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum - 1, 0);
        ASSERT_THROWS_CODE(session.onWriteOpCompletedOnPrimary(
                               opCtx(), txnNum - 1, {0}, opTime, Date_t::now(), boost::none),
                           AssertionException,
                           ErrorCodes::ConflictingOperationInProgress);
    }
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryForInvalidatedTransactionThrows) {
    const auto uuid = UUID::gen();
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);

    session.invalidate();

    ASSERT_THROWS_CODE(session.onWriteOpCompletedOnPrimary(
                           opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryCommitIgnoresInvalidation) {
    const auto uuid = UUID::gen();
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum);

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, uuid, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {0}, opTime, Date_t::now(), boost::none);

        session.invalidate();

        wuow.commit();
    }

    session.refreshFromStorageIfNeeded(opCtx());
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 0));
}

TEST_F(SessionTest, IncompleteHistoryDueToOpLogTruncation) {
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
    session.refreshFromStorageIfNeeded(opCtx());

    ASSERT_THROWS_CODE(session.checkStatementExecuted(opCtx(), txnNum, 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 1));
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 2));

    ASSERT_THROWS_CODE(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 0),
                       AssertionException,
                       ErrorCodes::IncompleteTransactionHistory);
    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 1));
    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 2));
}

TEST_F(SessionTest, ErrorOnlyWhenStmtIdBeingCheckedIsNotInCache) {
    const auto uuid = UUID::gen();
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 2;

    OperationSessionInfo osi;
    osi.setSessionId(sessionId);
    osi.setTxnNumber(txnNum);

    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());
    session.beginOrContinueTxn(opCtx(), txnNum);

    auto firstOpTime = ([&]() {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        const auto wallClockTime = Date_t::now();

        auto opTime = repl::logOp(opCtx(),
                                  "i",
                                  kNss,
                                  uuid,
                                  BSON("x" << 1),
                                  &Session::kDeadEndSentinel,
                                  false,
                                  wallClockTime,
                                  osi,
                                  1,
                                  {},
                                  false /* prepare */,
                                  OplogSlot());
        session.onWriteOpCompletedOnPrimary(
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
                                  &Session::kDeadEndSentinel,
                                  false,
                                  wallClockTime,
                                  osi,
                                  kIncompleteHistoryStmtId,
                                  link,
                                  false /* prepare */,
                                  OplogSlot());

        session.onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {kIncompleteHistoryStmtId}, opTime, wallClockTime, boost::none);
        wuow.commit();
    }

    {
        auto oplog = session.checkStatementExecuted(opCtx(), txnNum, 1);
        ASSERT_TRUE(oplog);
        ASSERT_EQ(firstOpTime, oplog->getOpTime());
    }

    ASSERT_THROWS(session.checkStatementExecuted(opCtx(), txnNum, 2), AssertionException);

    // Should have the same behavior after loading state from storage.
    session.invalidate();
    session.refreshFromStorageIfNeeded(opCtx());

    {
        auto oplog = session.checkStatementExecuted(opCtx(), txnNum, 1);
        ASSERT_TRUE(oplog);
        ASSERT_EQ(firstOpTime, oplog->getOpTime());
    }

    ASSERT_THROWS(session.checkStatementExecuted(opCtx(), txnNum, 2), AssertionException);
}

}  // namespace
}  // namespace mongo
