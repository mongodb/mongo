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
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const OptionalCollectionUUID kUUID;

class SessionTest : public MockReplCoordServerFixture {
protected:
    void setUp() final {
        MockReplCoordServerFixture::setUp();

        auto service = opCtx()->getServiceContext();
        SessionCatalog::reset_forTest(service);
        SessionCatalog::create(service);
        SessionCatalog::get(service)->onStepUp(opCtx());
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
    }

    static repl::OpTime logOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              StmtId stmtId) {
        return logOp(opCtx, nss, lsid, txnNumber, stmtId, {});
    }

    static repl::OpTime logOp(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const LogicalSessionId& lsid,
                              TxnNumber txnNumber,
                              StmtId stmtId,
                              repl::OpTime prevOpTime) {
        OperationSessionInfo osi;
        osi.setSessionId(lsid);
        osi.setTxnNumber(txnNumber);

        repl::OplogLink link;
        link.prevOpTime = prevOpTime;

        return repl::logOp(
            opCtx, "n", nss, kUUID, BSON("TestValue" << 0), nullptr, false, osi, stmtId, link);
    }
};

TEST_F(SessionTest, SessionEntryNotWrittenOnBegin) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    session.beginTxn(opCtx(), txnNum);

    ASSERT_EQ(sessionId, session.getSessionId());
    ASSERT(session.getLastWriteOpTime(txnNum).isNull());

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(!cursor->more());
}

TEST_F(SessionTest, SessionEntryWrittenAtFirstWrite) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 21;
    session.beginTxn(opCtx(), txnNum);

    const auto opTime = [&] {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime);
        wuow.commit();

        return opTime;
    }();

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(cursor->more());

    auto txnRecord = SessionTxnRecord::parse(
        IDLParserErrorContext("SessionEntryWrittenAtFirstWrite"), cursor->next());
    ASSERT(!cursor->more());
    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
    ASSERT_EQ(opTime, session.getLastWriteOpTime(txnNum));
}

TEST_F(SessionTest, StartingNewerTransactionUpdatesThePersistedSession) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const auto writeTxnRecordFn = [&](TxnNumber txnNum, StmtId stmtId, repl::OpTime prevOpTime) {
        session.beginTxn(opCtx(), txnNum);

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, stmtId, prevOpTime);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {stmtId}, opTime);
        wuow.commit();

        return opTime;
    };

    const auto firstOpTime = writeTxnRecordFn(100, 0, {});
    const auto secondOpTime = writeTxnRecordFn(200, 1, firstOpTime);

    DBDirectClient client(opCtx());
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                               {BSON("_id" << sessionId.toBSON())});
    ASSERT(cursor);
    ASSERT(cursor->more());

    auto txnRecord = SessionTxnRecord::parse(
        IDLParserErrorContext("SessionEntryWrittenAtFirstWrite"), cursor->next());
    ASSERT(!cursor->more());
    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(200, txnRecord.getTxnNum());
    ASSERT_EQ(secondOpTime, txnRecord.getLastWriteOpTime());
    ASSERT_EQ(secondOpTime, session.getLastWriteOpTime(200));

    session.invalidate();
    session.refreshFromStorageIfNeeded(opCtx());
    ASSERT_EQ(secondOpTime, session.getLastWriteOpTime(200));
}

TEST_F(SessionTest, StartingOldTxnShouldAssert) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    session.beginTxn(opCtx(), txnNum);

    ASSERT_THROWS_CODE(
        session.beginTxn(opCtx(), txnNum - 1), AssertionException, ErrorCodes::TransactionTooOld);
    ASSERT(session.getLastWriteOpTime(txnNum).isNull());
}

TEST_F(SessionTest, SessionTransactionsCollectionNotDefaultCreated) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    // Drop the transactions table
    BSONObj dropResult;
    DBDirectClient client(opCtx());
    const auto& nss = NamespaceString::kSessionTransactionsTableNamespace;
    ASSERT(client.runCommand(nss.db().toString(), BSON("drop" << nss.coll()), dropResult));

    const TxnNumber txnNum = 21;
    session.beginTxn(opCtx(), txnNum);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
    ASSERT_THROWS(session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime),
                  AssertionException);
}

TEST_F(SessionTest, CheckStatementExecuted) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginTxn(opCtx(), txnNum);

    const auto writeTxnRecordFn = [&](StmtId stmtId, repl::OpTime prevOpTime) {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, stmtId, prevOpTime);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {stmtId}, opTime);
        wuow.commit();

        return opTime;
    };

    ASSERT(!session.checkStatementExecuted(opCtx(), txnNum, 1000));
    const auto firstOpTime = writeTxnRecordFn(1000, {});
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 1000));

    ASSERT(!session.checkStatementExecuted(opCtx(), txnNum, 2000));
    writeTxnRecordFn(2000, firstOpTime);
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 2000));

    // Invalidate the session and ensure the statements still check out
    session.invalidate();
    session.refreshFromStorageIfNeeded(opCtx());

    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 2000));
}

TEST_F(SessionTest, CheckStatementExecutedForOldTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginTxn(opCtx(), txnNum);

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
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginTxn(opCtx(), txnNum);

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime);
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum - 1, 0);
        ASSERT_THROWS_CODE(session.onWriteOpCompletedOnPrimary(opCtx(), txnNum - 1, {0}, opTime),
                           AssertionException,
                           ErrorCodes::ConflictingOperationInProgress);
    }
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryForInvalidatedTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginTxn(opCtx(), txnNum);

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);

    session.invalidate();

    ASSERT_THROWS_CODE(session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryCommitIgnoresInvalidation) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginTxn(opCtx(), txnNum);

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime);

        session.invalidate();

        wuow.commit();
    }

    session.refreshFromStorageIfNeeded(opCtx());
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 0));
}

}  // namespace
}  // namespace mongo
