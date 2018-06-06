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
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const OptionalCollectionUUID kUUID;

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

class SessionTest : public MockReplCoordServerFixture {
protected:
    void setUp() final {
        MockReplCoordServerFixture::setUp();

        auto service = opCtx()->getServiceContext();
        SessionCatalog::get(service)->reset_forTest();
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

        return repl::logOp(opCtx,
                           "n",
                           nss,
                           kUUID,
                           BSON("TestValue" << 0),
                           nullptr,
                           false,
                           Date_t::now(),
                           osi,
                           stmtId,
                           link,
                           false /* prepare */);
    }

    void bumpTxnNumberFromDifferentOpCtx(Session* session, TxnNumber newTxnNum) {
        // Stash the original client.
        auto originalClient = Client::releaseCurrent();

        // Create a migration client and opCtx.
        auto service = opCtx()->getServiceContext();
        auto migrationClientOwned = service->makeClient("migrationClient");
        auto migrationClient = migrationClientOwned.get();
        Client::setCurrent(std::move(migrationClientOwned));
        auto migrationOpCtx = migrationClient->makeOperationContext();

        // Check that there is a transaction in progress with a lower txnNumber.
        ASSERT(session->inMultiDocumentTransaction());
        ASSERT_LT(session->getActiveTxnNumberForTest(), newTxnNum);

        // Check that the transaction has some operations, so we can ensure they are cleared.
        ASSERT_GT(session->transactionOperationsForTest().size(), 0u);

        // Bump the active transaction number on the session. This should clear all state from the
        // previous transaction.
        session->beginOrContinueTxnOnMigration(migrationOpCtx.get(), newTxnNum);
        ASSERT_EQ(session->getActiveTxnNumberForTest(), newTxnNum);
        ASSERT_FALSE(session->inMultiDocumentTransaction());
        ASSERT_FALSE(session->transactionIsAborted());
        ASSERT_EQ(session->transactionOperationsForTest().size(), 0u);

        // Restore the original client.
        migrationOpCtx.reset();
        Client::releaseCurrent();
        Client::setCurrent(std::move(originalClient));
    }
};

bool noopCursorExistsFunction(LogicalSessionId, TxnNumber) {
    return false;
};

TEST_F(SessionTest, SessionEntryNotWrittenOnBegin) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

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
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    const auto opTime = [&] {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime, Date_t::now());
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
        session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, stmtId, prevOpTime);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {stmtId}, opTime, Date_t::now());
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
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    ASSERT_THROWS_CODE(session.beginOrContinueTxn(
                           opCtx(), txnNum - 1, boost::none, boost::none, "testDB", "insert"),
                       AssertionException,
                       ErrorCodes::TransactionTooOld);
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
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
    ASSERT_THROWS(session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime, Date_t::now()),
                  AssertionException);
}

TEST_F(SessionTest, CheckStatementExecuted) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    const auto writeTxnRecordFn = [&](StmtId stmtId, repl::OpTime prevOpTime) {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, stmtId, prevOpTime);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {stmtId}, opTime, Date_t::now());
        wuow.commit();

        return opTime;
    };

    ASSERT(!session.checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(!session.checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));
    const auto firstOpTime = writeTxnRecordFn(1000, {});
    ASSERT(session.checkStatementExecuted(opCtx(), txnNum, 1000));
    ASSERT(session.checkStatementExecutedNoOplogEntryFetch(txnNum, 1000));

    ASSERT(!session.checkStatementExecuted(opCtx(), txnNum, 2000));
    ASSERT(!session.checkStatementExecutedNoOplogEntryFetch(txnNum, 2000));
    writeTxnRecordFn(2000, firstOpTime);
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
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

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
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime, Date_t::now());
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum - 1, 0);
        ASSERT_THROWS_CODE(
            session.onWriteOpCompletedOnPrimary(opCtx(), txnNum - 1, {0}, opTime, Date_t::now()),
            AssertionException,
            ErrorCodes::ConflictingOperationInProgress);
    }
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryForInvalidatedTransactionThrows) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);

    session.invalidate();

    ASSERT_THROWS_CODE(
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime, Date_t::now()),
        AssertionException,
        ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, WriteOpCompletedOnPrimaryCommitIgnoresInvalidation) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 100;
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        const auto opTime = logOp(opCtx(), kNss, sessionId, txnNum, 0);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {0}, opTime, Date_t::now());

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
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 2;

    OperationSessionInfo osi;
    osi.setSessionId(sessionId);
    osi.setTxnNumber(txnNum);

    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());
    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert");

    auto firstOpTime = ([&]() {
        AutoGetCollection autoColl(opCtx(), kNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        const auto wallClockTime = Date_t::now();

        auto opTime = repl::logOp(opCtx(),
                                  "i",
                                  kNss,
                                  kUUID,
                                  BSON("x" << 1),
                                  &Session::kDeadEndSentinel,
                                  false,
                                  wallClockTime,
                                  osi,
                                  1,
                                  {},
                                  false /* prepare */);
        session.onWriteOpCompletedOnPrimary(opCtx(), txnNum, {1}, opTime, wallClockTime);
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
                                  kUUID,
                                  {},
                                  &Session::kDeadEndSentinel,
                                  false,
                                  wallClockTime,
                                  osi,
                                  kIncompleteHistoryStmtId,
                                  link,
                                  false /* prepare */);

        session.onWriteOpCompletedOnPrimary(
            opCtx(), txnNum, {kIncompleteHistoryStmtId}, opTime, wallClockTime);
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

// Test that transaction operations will abort if locks cannot be taken immediately.
TEST_F(SessionTest, TransactionThrowsLockTimeoutIfLockIsUnavailable) {
    const std::string dbName = "TestDB";

    /**
     * Set up a transaction, take a database exclusive lock and then stash the transaction and
     * Client.
     */

    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 20;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");

    { Lock::DBLock dbXLock(opCtx(), dbName, MODE_X); }
    session.stashTransactionResources(opCtx());
    auto clientWithDatabaseXLock = Client::releaseCurrent();

    /**
     * Make a new Session, Client, OperationContext and transaction and then attempt to take the
     * same database exclusive lock, which should conflict because the other transaction already
     * took it.
     */

    auto service = opCtx()->getServiceContext();
    auto newClientOwned = service->makeClient("newTransactionClient");
    auto newClient = newClientOwned.get();
    Client::setCurrent(std::move(newClientOwned));
    auto newOpCtx = newClient->makeOperationContext();

    const auto newSessionId = makeLogicalSessionIdForTest();
    Session newSession(newSessionId);
    newSession.refreshFromStorageIfNeeded(newOpCtx.get());

    const TxnNumber newTxnNum = 10;
    newOpCtx.get()->setLogicalSessionId(newSessionId);
    newOpCtx.get()->setTxnNumber(newTxnNum);
    newSession.beginOrContinueTxn(newOpCtx.get(), newTxnNum, false, true, "testDB", "insert");
    newSession.unstashTransactionResources(newOpCtx.get(), "insert");

    ASSERT_THROWS_CODE(
        Lock::DBLock(newOpCtx.get(), dbName, MODE_X), AssertionException, ErrorCodes::LockTimeout);

    // A non-conflicting lock acquisition should work just fine.
    { Lock::DBLock(newOpCtx.get(), "NewTestDB", MODE_X); }

    // Restore the original client so that teardown works.
    newOpCtx.reset();
    Client::releaseCurrent();
    Client::setCurrent(std::move(clientWithDatabaseXLock));
}

TEST_F(SessionTest, StashAndUnstashResources) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    Locker* originalLocker = opCtx()->lockState();
    RecoveryUnit* originalRecoveryUnit = opCtx()->recoveryUnit();
    ASSERT(originalLocker);
    ASSERT(originalRecoveryUnit);

    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "find");

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    session.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());

    // Take a lock. This is expected in order to stash resources.
    Lock::GlobalRead lk(opCtx(), Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(lk.isLocked());

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    session.stashTransactionResources(opCtx());
    ASSERT_NOT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_NOT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    session.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, opCtx()->lockState());
    ASSERT_EQUALS(originalRecoveryUnit, opCtx()->recoveryUnit());
    ASSERT(opCtx()->getWriteUnitOfWork());

    // Commit the transaction. This allows us to release locks.
    session.commitTransaction(opCtx());
}

TEST_F(SessionTest, ReportStashedResources) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "find");

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    session.unstashTransactionResources(opCtx(), "find");
    ASSERT(opCtx()->getWriteUnitOfWork());

    // Take a lock. This is expected in order to stash resources.
    Lock::GlobalRead lk(opCtx(), Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(lk.isLocked());

    // Build a BSONObj containing the details which we expect to see reported when we call
    // Session::reportStashedState.
    const auto lockerInfo = opCtx()->lockState()->getLockerInfo();
    ASSERT(lockerInfo);

    auto txnDoc = BSON("parameters" << BSON("txnNumber" << txnNum));
    auto reportBuilder =
        std::move(BSONObjBuilder() << "host" << getHostNameCachedAndPort() << "desc"
                                   << "inactive transaction"
                                   << "lsid"
                                   << sessionId.toBSON()
                                   << "transaction"
                                   << txnDoc
                                   << "waitingForLock"
                                   << false
                                   << "active"
                                   << false);
    fillLockerInfo(*lockerInfo, reportBuilder);

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    session.stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Verify that the Session's report of its own stashed state aligns with our expectations.
    ASSERT_BSONOBJ_EQ(session.reportStashedState(), reportBuilder.obj());

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    session.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT(opCtx()->getWriteUnitOfWork());

    // With the resources unstashed, verify that the Session reports an empty stashed state.
    ASSERT(session.reportStashedState().isEmpty());

    // Commit the transaction. This allows us to release locks.
    session.commitTransaction(opCtx());
}

TEST_F(SessionTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    // Autocommit should be true by default
    ASSERT(session.getAutocommit());

    const TxnNumber txnNum = 100;
    // Must specify startTransaction=true and autocommit=false to start a transaction.
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Autocommit should be set to false and we should be in a mult-doc transaction.
    ASSERT_FALSE(session.getAutocommit());
    ASSERT_TRUE(session.inSnapshotReadOrMultiDocumentTransaction());

    // Cannot try to start a transaction that already started.
    ASSERT_THROWS_CODE(session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert"),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, AutocommitRequiredOnEveryTxnOp) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    // Autocommit should be true by default
    ASSERT(session.getAutocommit());

    const TxnNumber txnNum = 100;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // We must have stashed transaction resources to do a second operation on the transaction.
    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // Autocommit should be set to false
    ASSERT_FALSE(session.getAutocommit());

    // Omitting 'autocommit' after the first statement of a transaction should throw an error.
    ASSERT_THROWS_CODE(
        session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "insert"),
        AssertionException,
        ErrorCodes::InvalidOptions);

    // Setting 'autocommit=true' should throw an error.
    ASSERT_THROWS_CODE(
        session.beginOrContinueTxn(opCtx(), txnNum, true, boost::none, "testDB", "insert"),
        AssertionException,
        ErrorCodes::InvalidOptions);

    // Including autocommit=false should succeed.
    session.beginOrContinueTxn(opCtx(), txnNum, false, boost::none, "testDB", "insert");
}

TEST_F(SessionTest, SameTransactionPreservesStoredStatements) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 22;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // We must have stashed transaction resources to re-open the transaction.
    session.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(), session.transactionOperationsForTest()[0].toBSON());
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // Check the transaction operations before re-opening the transaction.
    ASSERT_BSONOBJ_EQ(operation.toBSON(), session.transactionOperationsForTest()[0].toBSON());

    // Re-opening the same transaction should have no effect.
    session.beginOrContinueTxn(opCtx(), txnNum, false, boost::none, "testDB", "insert");
    ASSERT_BSONOBJ_EQ(operation.toBSON(), session.transactionOperationsForTest()[0].toBSON());
}

TEST_F(SessionTest, AbortClearsStoredStatements) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 24;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(), session.transactionOperationsForTest()[0].toBSON());
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    session.abortArbitraryTransaction();
    ASSERT_TRUE(session.transactionOperationsForTest().empty());
    ASSERT_TRUE(session.transactionIsAborted());
}

// This test makes sure the commit machinery works even when no operations are done on the
// transaction.
TEST_F(SessionTest, EmptyTransactionCommit) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 25;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    session.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    session.commitTransaction(opCtx());
    session.stashTransactionResources(opCtx());
    ASSERT_TRUE(session.transactionIsCommitted());
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(SessionTest, EmptyTransactionAbort) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "abortTransaction");
    session.unstashTransactionResources(opCtx(), "abortTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    session.abortArbitraryTransaction();
    ASSERT_TRUE(session.transactionIsAborted());
}

TEST_F(SessionTest, ConcurrencyOfUnstashAndAbort) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "find");

    // The transaction may be aborted without checking out the session.
    session.abortArbitraryTransaction();

    // An unstash after an abort should uassert.
    ASSERT_THROWS_CODE(session.unstashTransactionResources(opCtx(), "find"),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(SessionTest, ConcurrencyOfUnstashAndMigration) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);
    session.stashTransactionResources(opCtx());

    // A migration may bump the active transaction number without checking out the session.
    const TxnNumber higherTxnNum = 27;
    bumpTxnNumberFromDifferentOpCtx(&session, higherTxnNum);

    // An unstash after a migration that bumps the active transaction number should uassert.
    ASSERT_THROWS_CODE(session.unstashTransactionResources(opCtx(), "insert"),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, ConcurrencyOfStashAndAbort) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "find");

    session.unstashTransactionResources(opCtx(), "find");

    // The transaction may be aborted without checking out the session.
    session.abortArbitraryTransaction();

    // A stash after an abort should be a noop.
    session.stashTransactionResources(opCtx());
}

TEST_F(SessionTest, ConcurrencyOfStashAndMigration) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the session.
    const TxnNumber higherTxnNum = 27;
    bumpTxnNumberFromDifferentOpCtx(&session, higherTxnNum);

    // A stash after a migration that bumps the active transaction number should uassert.
    ASSERT_THROWS_CODE(session.stashTransactionResources(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, ConcurrencyOfAddTransactionOperationAndAbort) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");

    // The transaction may be aborted without checking out the session.
    session.abortArbitraryTransaction();

    // An addTransactionOperation() after an abort should uassert.
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    ASSERT_THROWS_CODE(session.addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(SessionTest, ConcurrencyOfAddTransactionOperationAndMigration) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "find");

    session.unstashTransactionResources(opCtx(), "find");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the session.
    const TxnNumber higherTxnNum = 27;
    bumpTxnNumberFromDifferentOpCtx(&session, higherTxnNum);

    // An addTransactionOperation() after a migration that bumps the active transaction number
    // should uassert.
    ASSERT_THROWS_CODE(session.addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, ConcurrencyOfEndTransactionAndRetrieveOperationsAndAbort) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");

    // The transaction may be aborted without checking out the session.
    session.abortArbitraryTransaction();

    // An endTransactionAndRetrieveOperations() after an abort should uassert.
    ASSERT_THROWS_CODE(session.endTransactionAndRetrieveOperations(opCtx()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(SessionTest, ConcurrencyOfEndTransactionAndRetrieveOperationsAndMigration) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the session.
    const TxnNumber higherTxnNum = 27;
    bumpTxnNumberFromDifferentOpCtx(&session, higherTxnNum);

    // An endTransactionAndRetrieveOperations() after a migration that bumps the active transaction
    // number should uassert.
    ASSERT_THROWS_CODE(session.endTransactionAndRetrieveOperations(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(SessionTest, ConcurrencyOfCommitTransactionAndAbort) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");

    session.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction may be aborted without checking out the session.
    session.abortArbitraryTransaction();

    // An commitTransaction() after an abort should uassert.
    ASSERT_THROWS_CODE(
        session.commitTransaction(opCtx()), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(SessionTest, ConcurrencyOfCommitTransactionAndMigration) {
    Session::registerCursorExistsFunction(noopCursorExistsFunction);
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 26;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::OplogEntry::makeInsertOperation(kNss, kUUID, BSON("TestValue" << 0));
    session.addTransactionOperation(opCtx(), operation);

    // A migration may bump the active transaction number without checking out the session.
    const TxnNumber higherTxnNum = 27;
    bumpTxnNumberFromDifferentOpCtx(&session, higherTxnNum);

    // An commitTransaction() after a migration that bumps the active transaction number should
    // uassert.
    ASSERT_THROWS_CODE(session.commitTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

// Tests that a transaction aborts if it becomes too large before trying to commit it.
TEST_F(SessionTest, TransactionTooLargeWhileBuilding) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 28;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "insert");

    // Two 6MB operations should succeed; three 6MB operations should fail.
    constexpr size_t kBigDataSize = 6 * 1024 * 1024;
    std::unique_ptr<uint8_t[]> bigData(new uint8_t[kBigDataSize]());
    auto operation = repl::OplogEntry::makeInsertOperation(
        kNss,
        kUUID,
        BSON("_id" << 0 << "data" << BSONBinData(bigData.get(), kBigDataSize, BinDataGeneral)));
    session.addTransactionOperation(opCtx(), operation);
    session.addTransactionOperation(opCtx(), operation);
    ASSERT_THROWS_CODE(session.addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
}

}  // namespace
}  // namespace mongo
