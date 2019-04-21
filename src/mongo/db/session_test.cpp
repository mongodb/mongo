
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
#include "mongo/logger/logger.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
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
                           OplogSlot());
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
        ASSERT(session->inActiveOrKilledMultiDocumentTransaction());
        ASSERT_FALSE(session->transactionIsAborted());
        ASSERT_LT(session->getActiveTxnNumberForTest(), newTxnNum);

        // Check that the transaction has some operations, so we can ensure they are cleared.
        ASSERT_GT(session->transactionOperationsForTest().size(), 0u);

        // Bump the active transaction number on the session. This should clear all state from the
        // previous transaction.
        session->beginOrContinueTxnOnMigration(migrationOpCtx.get(), newTxnNum);
        ASSERT_EQ(session->getActiveTxnNumberForTest(), newTxnNum);
        ASSERT_FALSE(session->inActiveOrKilledMultiDocumentTransaction());
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
                                  OplogSlot());
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
                                  OplogSlot());

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

// Test that transaction lock acquisition times out in `maxTransactionLockRequestTimeoutMillis`
// milliseconds.
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

    Date_t t1 = Date_t::now();
    ASSERT_THROWS_CODE(
        Lock::DBLock(newOpCtx.get(), dbName, MODE_X), AssertionException, ErrorCodes::LockTimeout);
    Date_t t2 = Date_t::now();
    int defaultMaxTransactionLockRequestTimeoutMillis = 5;
    ASSERT_GTE(t2 - t1, Milliseconds(defaultMaxTransactionLockRequestTimeoutMillis));

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
    ASSERT(opCtx()->lockState()->isLocked());

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
    Date_t startTime = Date_t::now();
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    const bool autocommit = false;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               "appName",
                                               &builder));
    auto obj = builder.obj();
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    session.beginOrContinueTxn(opCtx(), txnNum, autocommit, true, "testDB", "find");

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
    ASSERT(opCtx()->lockState()->isLocked());

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    session.stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // Verify that the Session's report of its own stashed state aligns with our expectations.
    auto stashedState = session.reportStashedState();
    auto transactionDocument = stashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(stashedState.getField("host").valueStringData().toString(),
              getHostNameCachedAndPort());
    ASSERT_EQ(stashedState.getField("desc").valueStringData().toString(), "inactive transaction");
    ASSERT_BSONOBJ_EQ(stashedState.getField("lsid").Obj(), sessionId.toBSON());
    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), txnNum);
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_GTE(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        Date_t::fromMillisSinceEpoch(session.getSingleTransactionStats()->getStartTime() / 1000) +
            Seconds{transactionLifetimeLimitSeconds.load()});
    ASSERT_EQ(stashedState.getField("client").valueStringData().toString(), "");
    ASSERT_EQ(stashedState.getField("connectionId").numberLong(), 0);
    ASSERT_EQ(stashedState.getField("appName").valueStringData().toString(), "appName");
    ASSERT_BSONOBJ_EQ(stashedState.getField("clientMetadata").Obj(), obj.getField("client").Obj());
    ASSERT_EQ(stashedState.getField("waitingForLock").boolean(), false);
    ASSERT_EQ(stashedState.getField("active").boolean(), false);
    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that the
    // values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

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

TEST_F(SessionTest, ReportUnstashedResources) {
    Date_t startTime = Date_t::now();
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    const bool autocommit = false;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    session.beginOrContinueTxn(opCtx(), txnNum, autocommit, true, "testDB", "find");

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
    ASSERT(opCtx()->lockState()->isLocked());

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    session.reportUnstashedState(repl::ReadConcernArgs::get(opCtx()), &unstashedStateBuilder);
    auto unstashedState = unstashedStateBuilder.obj();
    auto transactionDocument = unstashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), txnNum);
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_GTE(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        Date_t::fromMillisSinceEpoch(session.getSingleTransactionStats()->getStartTime() / 1000) +
            Seconds{transactionLifetimeLimitSeconds.load()});
    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that the
    // values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    session.stashTransactionResources(opCtx());
    ASSERT(!opCtx()->getWriteUnitOfWork());

    // With the resources stashed, verify that the Session reports an empty unstashed state.
    BSONObjBuilder builder;
    session.reportUnstashedState(repl::ReadConcernArgs::get(opCtx()), &builder);
    ASSERT(builder.obj().isEmpty());
}

TEST_F(SessionTest, ReportUnstashedResourcesForARetryableWrite) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    ASSERT(opCtx()->lockState());
    ASSERT(opCtx()->recoveryUnit());

    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    session.beginOrContinueTxn(opCtx(), txnNum, boost::none, boost::none, "testDB", "find");
    session.unstashTransactionResources(opCtx(), "find");

    // Build a BSONObj containing the details which we expect to see reported when we call
    // Session::reportUnstashedState. For a retryable write, we should only include the txnNumber.
    BSONObjBuilder reportBuilder;
    BSONObjBuilder transactionBuilder(reportBuilder.subobjStart("transaction"));
    BSONObjBuilder parametersBuilder(transactionBuilder.subobjStart("parameters"));
    parametersBuilder.append("txnNumber", txnNum);
    parametersBuilder.done();
    transactionBuilder.done();

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    session.reportUnstashedState(repl::ReadConcernArgs::get(opCtx()), &unstashedStateBuilder);
    ASSERT_BSONOBJ_EQ(unstashedStateBuilder.obj(), reportBuilder.obj());
}

TEST_F(SessionTest, CannotSpecifyStartTransactionOnInProgressTxn) {
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
    ASSERT(session.inActiveOrKilledMultiDocumentTransaction());
    ASSERT_FALSE(session.transactionIsAborted());

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

TEST_F(SessionTest, IncrementTotalStartedUponStartTransaction) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getTotalStarted();

    const TxnNumber txnNum = 1;
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Tests that the total transactions started counter is incremented by 1 when a new transaction
    // is started.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalStarted(),
              beforeTransactionStart + 1U);
}

TEST_F(SessionTest, IncrementTotalCommittedOnCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    session.unstashTransactionResources(opCtx(), "commitTransaction");

    unsigned long long beforeCommitCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalCommitted();

    session.commitTransaction(opCtx());

    // Assert that the committed counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalCommitted(), beforeCommitCount + 1U);
}

TEST_F(SessionTest, IncrementTotalAbortedUponAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");

    unsigned long long beforeAbortCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    session.abortArbitraryTransaction();

    // Assert that the aborted counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortCount + 1U);
}

TEST_F(SessionTest, TrackTotalOpenTransactionsWithAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Tests that starting a transaction increments the open transactions counter by 1.
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that aborting a transaction decrements the open transactions counter by 1.
    session.abortArbitraryTransaction();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(SessionTest, TrackTotalOpenTransactionsWithCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Tests that starting a transaction increments the open transactions counter by 1.
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    session.unstashTransactionResources(opCtx(), "insert");

    // Tests that committing a transaction decrements the open transactions counter by 1.
    session.commitTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(SessionTest, TrackTotalActiveAndInactiveTransactionsWithCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    session.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that the second unstash increments the active counter and decrements the inactive
    // counter.
    session.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that committing a transaction decrements the active counter only.
    session.commitTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(SessionTest, TrackTotalActiveAndInactiveTransactionsWithStashedAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    session.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that aborting a stashed transaction decrements the inactive counter only.
    session.abortArbitraryTransaction();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(SessionTest, TrackTotalActiveAndInactiveTransactionsWithUnstashedAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    session.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that aborting a stashed transaction decrements the active counter only.
    session.abortArbitraryTransaction();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(SessionTest, TransactionErrorsBeforeUnstash) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    unsigned long long beforeAbortedCounter =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    // The first transaction statement checks out the session and begins the transaction but returns
    // before unstashTransactionResources().
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // The transaction is now inactive.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortedCounter);

    // The second transaction statement continues the transaction. Since there are no stashed
    // transaction resources, it is not safe to continue the transaction, so the transaction is
    // aborted.
    ASSERT_THROWS_CODE(
        session.beginOrContinueTxn(opCtx(), txnNum, false, boost::none, "testDB", "insert"),
        AssertionException,
        ErrorCodes::NoSuchTransaction);

    // The transaction is now aborted.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(),
              beforeAbortedCounter + 1U);
}

/**
 * Test fixture for transactions metrics.
 */
class TransactionsMetricsTest : public SessionTest {};

TEST_F(TransactionsMetricsTest, SingleTransactionStatsStartTimeShouldBeSetUponTransactionStart) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;

    // Save the time before the transaction is created.
    unsigned long long timeBeforeTxn = curTimeMicros64();
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    unsigned long long timeAfterTxn = curTimeMicros64();

    // Start time should be greater than or equal to the time before the transaction was created.
    ASSERT_GTE(session.getSingleTransactionStats()->getStartTime(), timeBeforeTxn);

    // Start time should be less than or equal to the time after the transaction was started.
    ASSERT_LTE(session.getSingleTransactionStats()->getStartTime(), timeAfterTxn);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    unsigned long long timeBeforeTxnStart = curTimeMicros64();
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    unsigned long long timeAfterTxnStart = curTimeMicros64();
    session.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Sleep here to allow enough time to elapse.
    sleepmillis(10);

    unsigned long long timeBeforeTxnCommit = curTimeMicros64();
    session.commitTransaction(opCtx());
    unsigned long long timeAfterTxnCommit = curTimeMicros64();

    ASSERT_GTE(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
               timeBeforeTxnCommit - timeAfterTxnStart);

    ASSERT_LTE(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
               timeAfterTxnCommit - timeBeforeTxnStart);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    unsigned long long timeBeforeTxnStart = curTimeMicros64();
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    unsigned long long timeAfterTxnStart = curTimeMicros64();
    session.unstashTransactionResources(opCtx(), "insert");

    // Sleep here to allow enough time to elapse.
    sleepmillis(10);

    unsigned long long timeBeforeTxnAbort = curTimeMicros64();
    session.abortArbitraryTransaction();
    unsigned long long timeAfterTxnAbort = curTimeMicros64();

    ASSERT_GTE(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
               timeBeforeTxnAbort - timeAfterTxnStart);

    ASSERT_LTE(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
               timeAfterTxnAbort - timeBeforeTxnStart);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    session.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Save the transaction's duration at this point.
    unsigned long long txnDurationAfterStart =
        session.getSingleTransactionStats()->getDuration(curTimeMicros64());
    sleepmillis(10);

    // The transaction's duration should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
              txnDurationAfterStart);
    sleepmillis(10);
    session.commitTransaction(opCtx());
    unsigned long long txnDurationAfterCommit =
        session.getSingleTransactionStats()->getDuration(curTimeMicros64());

    // The transaction has committed, so the duration should have not increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
              txnDurationAfterCommit);

    ASSERT_GT(txnDurationAfterCommit, txnDurationAfterStart);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);

    // Save the transaction's duration at this point.
    unsigned long long txnDurationAfterStart =
        session.getSingleTransactionStats()->getDuration(curTimeMicros64());
    sleepmillis(10);

    // The transaction's duration should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
              txnDurationAfterStart);
    sleepmillis(10);
    session.abortArbitraryTransaction();
    unsigned long long txnDurationAfterAbort =
        session.getSingleTransactionStats()->getDuration(curTimeMicros64());

    // The transaction has aborted, so the duration should have not increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getDuration(curTimeMicros64()),
              txnDurationAfterAbort);

    ASSERT_GT(txnDurationAfterAbort, txnDurationAfterStart);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndStash) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time active should be zero.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    session.unstashTransactionResources(opCtx(), "insert");

    // Sleep a bit to make sure time active is nonzero.
    sleepmillis(1);

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // Time active should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());

    session.unstashTransactionResources(opCtx(), "insert");
    // Sleep here to allow enough time to elapse.
    sleepmillis(10);
    session.stashTransactionResources(opCtx());

    // Time active should have increased again.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);

    // Start a new transaction.
    const TxnNumber higherTxnNum = 2;
    session.beginOrContinueTxn(opCtx(), higherTxnNum, false, true, "testDB", "insert");

    // Time active should be zero for a new transaction.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time active should be zero.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    session.unstashTransactionResources(opCtx(), "insert");
    // Sleep here to allow enough time to elapse.
    sleepmillis(10);
    session.abortArbitraryTransaction();

    // Time active should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());

    // The transaction is no longer active, so time active should not have increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetUponAbortOnly) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time active should be zero.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    session.abortArbitraryTransaction();

    // Time active should not have increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilStash) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time active should be zero.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
    session.unstashTransactionResources(opCtx(), "insert");
    sleepmillis(1);

    // Time active should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);

    // Time active should have increased again.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // The transaction is no longer active, so time active should not have increased.
    timeActiveSoFar = session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");

    // Time active should be zero.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});
    session.unstashTransactionResources(opCtx(), "commitTransaction");
    sleepmillis(1);

    // Time active should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);

    // Time active should have increased again.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
    session.commitTransaction(opCtx());

    // The transaction is no longer active, so time active should not have increased.
    timeActiveSoFar = session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());
    sleepmillis(1);
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetIfUnstashHasBadReadConcernArgs) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "find");

    // Initialize bad read concern args (!readConcernArgs.isEmpty()).
    repl::ReadConcernArgs readConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Transaction resources do not exist yet.
    session.unstashTransactionResources(opCtx(), "find");

    // Sleep a bit to make sure time active is nonzero.
    sleepmillis(1);

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // Time active should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              Microseconds{0});

    // Save time active at this point.
    auto timeActiveSoFar =
        session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64());

    // Transaction resources already exist here and should throw an exception due to bad read
    // concern arguments.
    ASSERT_THROWS_CODE(session.unstashTransactionResources(opCtx(), "find"),
                       AssertionException,
                       ErrorCodes::InvalidOptions);

    // Time active should not have increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeActiveMicros(curTimeMicros64()),
              timeActiveSoFar);
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponStash) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Initialize field values for both AdditiveMetrics objects.
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysExamined = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 5;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.docsExamined = 2;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 0;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nMatched = 3;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 4;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nmoved = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.nmoved = 2;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysInserted = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysDeleted = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.prepareReadConflicts = 5;
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts = 4;

    auto additiveMetricsToCompare =
        session.getSingleTransactionStats()->getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    ASSERT(session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Initialize field values for both AdditiveMetrics objects.
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysExamined = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 2;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.docsExamined = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 2;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nMatched = 4;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nModified = 5;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 1;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.ndeleted = 4;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 0;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysInserted = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.prepareReadConflicts = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts = 0;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.writeConflicts = 6;
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts = 3;

    auto additiveMetricsToCompare =
        session.getSingleTransactionStats()->getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.commitTransaction(opCtx());

    ASSERT(session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Initialize field values for both AdditiveMetrics objects.
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysExamined = 2;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 4;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.docsExamined = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 3;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nMatched = 2;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nModified = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 5;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.nmoved = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.nmoved = 2;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysInserted = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.keysDeleted = 6;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.writeConflicts = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts = 3;

    auto additiveMetricsToCompare =
        session.getSingleTransactionStats()->getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.abortActiveTransaction(opCtx());

    ASSERT(session.getSingleTransactionStats()->getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndStash) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time inactive should be greater than or equal to zero.
    ASSERT_GTE(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
               Microseconds{0});

    // Save time inactive at this point.
    auto timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // Time inactive should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction is still inactive, so time inactive should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    session.unstashTransactionResources(opCtx(), "insert");

    timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction is currently active, so time inactive should not have increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // The transaction is inactive again, so time inactive should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time inactive should be greater than or equal to zero.
    ASSERT_GTE(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
               Microseconds{0});

    // Save time inactive at this point.
    auto timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // Time inactive should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    session.unstashTransactionResources(opCtx(), "insert");
    session.abortArbitraryTransaction();

    timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction has aborted, so time inactive should not have increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldIncreaseUntilCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);
    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    // Time inactive should be greater than or equal to zero.
    ASSERT_GTE(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
               Microseconds{0});

    // Save time inactive at this point.
    auto timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // Time inactive should have increased.
    ASSERT_GT(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);

    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.commitTransaction(opCtx());

    timeInactiveSoFar =
        session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64());
    // Sleep here to allow enough time to elapse.
    sleepmillis(1);

    // The transaction has committed, so time inactive should not have increased.
    ASSERT_EQ(session.getSingleTransactionStats()->getTimeInactiveMicros(curTimeMicros64()),
              timeInactiveSoFar);
}

namespace {

/*
 * Constructs a ClientMetadata BSONObj with the given application name.
 */
BSONObj constructClientMetadata(StringData appName) {
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               appName,
                                               &builder));
    return builder.obj();
}
}  // namespace

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponStash) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());

    // LastClientInfo should have been set.
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientHostAndPort, "");
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().connectionId, 0);
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().appName, "appName");
    ASSERT_BSONOBJ_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientMetadata,
                      obj.getField("client").Obj());

    // Create another ClientMetadata object.
    auto newObj = constructClientMetadata("newAppName");
    auto newClientMetadata = ClientMetadata::parse(newObj["client"]);
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(newClientMetadata.getValue()));

    session.unstashTransactionResources(opCtx(), "insert");
    session.stashTransactionResources(opCtx());

    // LastClientInfo's clientMetadata should have been updated to the new ClientMetadata object.
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().appName, "newAppName");
    ASSERT_BSONOBJ_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientMetadata,
                      newObj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    session.commitTransaction(opCtx());

    // LastClientInfo should have been set.
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientHostAndPort, "");
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().connectionId, 0);
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().appName, "appName");
    ASSERT_BSONOBJ_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientMetadata,
                      obj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Create a ClientMetadata object and set it on ClientMetadataIsMasterState.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);

    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx()->getClient());
    clientMetadataIsMasterState.setClientMetadata(opCtx()->getClient(),
                                                  std::move(clientMetadata.getValue()));

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");
    session.unstashTransactionResources(opCtx(), "insert");
    session.abortActiveTransaction(opCtx());

    // LastClientInfo should have been set.
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientHostAndPort, "");
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().connectionId, 0);
    ASSERT_EQ(session.getSingleTransactionStats()->getLastClientInfo().appName, "appName");
    ASSERT_BSONOBJ_EQ(session.getSingleTransactionStats()->getLastClientInfo().clientMetadata,
                      obj.getField("client").Obj());
}

namespace {

/*
 * Sets up the additive metrics for Transactions Metrics test.
 */
void setupAdditiveMetrics(const int metricValue, OperationContext* opCtx) {
    CurOp::get(opCtx)->debug().additiveMetrics.keysExamined = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.docsExamined = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nMatched = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nModified = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.ninserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.ndeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nmoved = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysInserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysDeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.prepareReadConflicts = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.writeConflicts = metricValue;
}

/*
 * Builds expected parameters info string.
 */
void buildParametersInfoString(StringBuilder* sb,
                               LogicalSessionId sessionId,
                               const TxnNumber txnNum,
                               const repl::ReadConcernArgs readConcernArgs) {
    BSONObjBuilder lsidBuilder;
    sessionId.serialize(&lsidBuilder);
    (*sb) << "parameters:{ lsid: " << lsidBuilder.done().toString() << ", txnNumber: " << txnNum
          << ", autocommit: false"
          << ", readConcern: " << readConcernArgs.toBSON().getObjectField("readConcern") << " },";
}

/*
 * Builds expected single transaction stats info string.
 */
void buildSingleTransactionStatsString(StringBuilder* sb, const int metricValue) {
    (*sb) << " keysExamined:" << metricValue << " docsExamined:" << metricValue
          << " nMatched:" << metricValue << " nModified:" << metricValue
          << " ninserted:" << metricValue << " ndeleted:" << metricValue
          << " nmoved:" << metricValue << " keysInserted:" << metricValue
          << " keysDeleted:" << metricValue << " prepareReadConflicts:" << metricValue
          << " writeConflicts:" << metricValue;
}

/*
 * Builds the time active and time inactive info string.
 */
void buildTimeActiveInactiveString(StringBuilder* sb,
                                   Session* session,
                                   unsigned long long curTime) {
    // Add time active micros to string.
    (*sb) << " timeActiveMicros:"
          << durationCount<Microseconds>(
                 session->getSingleTransactionStats()->getTimeActiveMicros(curTime));

    // Add time inactive micros to string.
    (*sb) << " timeInactiveMicros:"
          << durationCount<Microseconds>(
                 session->getSingleTransactionStats()->getTimeInactiveMicros(curTime));
}


/*
 * Builds the entire expected transaction info string and returns it.
 */
std::string buildTransactionInfoString(OperationContext* opCtx,
                                       Session* session,
                                       std::string terminationCause,
                                       const LogicalSessionId sessionId,
                                       const TxnNumber txnNum,
                                       const int metricValue) {
    // Calling transactionInfoForLog to get the actual transaction info string.
    const auto lockerInfo = opCtx->lockState()->getLockerInfo(boost::none);

    // Building expected transaction info string.
    StringBuilder parametersInfo;
    buildParametersInfoString(
        &parametersInfo, sessionId, txnNum, repl::ReadConcernArgs::get(opCtx));

    StringBuilder readTimestampInfo;
    readTimestampInfo
        << " readTimestamp:"
        << session->getSpeculativeTransactionReadOpTimeForTest().getTimestamp().toString() << ",";

    StringBuilder singleTransactionStatsInfo;
    buildSingleTransactionStatsString(&singleTransactionStatsInfo, metricValue);

    auto curTime = curTimeMicros64();
    StringBuilder timeActiveAndInactiveInfo;
    buildTimeActiveInactiveString(&timeActiveAndInactiveInfo, session, curTime);

    BSONObjBuilder locks;
    if (lockerInfo) {
        lockerInfo->stats.report(&locks);
    }

    // Puts all the substrings together into one expected info string. The expected info string will
    // look something like this:
    // parameters:{ lsid: { id: UUID("f825288c-100e-49a1-9fd7-b95c108049e6"), uid: BinData(0,
    // E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855) }, txnNumber: 1,
    // autocommit: false }, readTimestamp:Timestamp(0, 0), keysExamined:1 docsExamined:1 nMatched:1
    // nModified:1 ninserted:1 ndeleted:1 nmoved:1 keysInserted:1 keysDeleted:1
    // prepareReadConflicts:1 writeConflicts:1 terminationCause:committed timeActiveMicros:3
    // timeInactiveMicros:2 numYields:0 locks:{ Global: { acquireCount: { r: 6, w: 4 } }, Database:
    // { acquireCount: { r: 1, w: 1, W: 2 } }, Collection: { acquireCount: { R: 1 } }, oplog: {
    // acquireCount: { W: 1 } } } 0ms
    StringBuilder expectedTransactionInfo;
    expectedTransactionInfo << parametersInfo.str() << readTimestampInfo.str()
                            << singleTransactionStatsInfo.str()
                            << " terminationCause:" << terminationCause
                            << timeActiveAndInactiveInfo.str() << " numYields:" << 0
                            << " locks:" << locks.done().toString() << " "
                            << Milliseconds{
                                   static_cast<long long>(
                                       session->getSingleTransactionStats()->getDuration(curTime)) /
                                   1000};
    return expectedTransactionInfo.str();
}
}  // namespace

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    session.unstashTransactionResources(opCtx(), "commitTransaction");
    session.commitTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string testTransactionInfo =
        session.transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(), &session, "committed", sessionId, txnNum, metricValue);
    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "abortTransaction");

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    session.unstashTransactionResources(opCtx(), "abortTransaction");
    session.abortActiveTransaction(opCtx());

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string testTransactionInfo =
        session.transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(), &session, "aborted", sessionId, txnNum, metricValue);
    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

DEATH_TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogWithNoLockerInfoStats, "invariant") {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "testDB", "insert");

    session.unstashTransactionResources(opCtx(), "commitTransaction");
    session.commitTransaction(opCtx());

    session.transactionInfoForLogForTest(nullptr, true, readConcernArgs);
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowCommit) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    session.unstashTransactionResources(opCtx(), "commitTransaction");

    // Sleep generously longer than the slowMS value.
    serverGlobalParams.slowMS = 1;
    sleepmillis(5 * serverGlobalParams.slowMS);

    startCapturingLogMessages();
    session.commitTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        session.transactionInfoForLogForTest(&lockerInfo->stats, true, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "abortTransaction");
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    session.unstashTransactionResources(opCtx(), "abortTransaction");

    // Sleep generously longer than the slowMS value.
    serverGlobalParams.slowMS = 1;
    sleepmillis(5 * serverGlobalParams.slowMS);

    startCapturingLogMessages();
    session.abortActiveTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = opCtx()->lockState()->getLockerInfo(boost::none);
    ASSERT(lockerInfo);
    std::string expectedTransactionInfo = "transaction " +
        session.transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowStashedAbort) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(BSON("find"
                                              << "test"
                                              << repl::ReadConcernArgs::kReadConcernFieldName
                                              << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                      << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "abortTransaction");
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    session.unstashTransactionResources(opCtx(), "insert");
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    session.stashTransactionResources(opCtx());
    const auto txnResourceStashLocker = session.getTxnResourceStashLockerForTest();
    ASSERT(txnResourceStashLocker);
    const auto lockerInfo = txnResourceStashLocker->getLockerInfo(boost::none);

    // Sleep generously longer than the slowMS value.
    serverGlobalParams.slowMS = 1;
    sleepmillis(5 * serverGlobalParams.slowMS);

    startCapturingLogMessages();
    session.abortArbitraryTransaction();
    stopCapturingLogMessages();

    std::string expectedTransactionInfo = "transaction " +
        session.transactionInfoForLogForTest(&lockerInfo->stats, false, readConcernArgs);
    ASSERT_EQUALS(1, countLogLinesContaining(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoVerbosityInfo) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Set a high slow operation threshold to avoid the transaction being logged as slow.
    serverGlobalParams.slowMS = 10000;

    // Set verbosity level of transaction components to info.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Info());

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    session.unstashTransactionResources(opCtx(), "commitTransaction");

    startCapturingLogMessages();
    session.commitTransaction(opCtx());
    stopCapturingLogMessages();

    // Test that the transaction is not logged.
    ASSERT_EQUALS(0, countLogLinesContaining("transaction parameters"));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoVerbosityDebug) {
    const auto sessionId = makeLogicalSessionIdForTest();
    Session session(sessionId);
    session.refreshFromStorageIfNeeded(opCtx());

    const TxnNumber txnNum = 1;
    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    // Set a high slow operation threshold to avoid the transaction being logged as slow.
    serverGlobalParams.slowMS = 10000;

    // Set verbosity level of transaction components to debug.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Debug(1));

    session.beginOrContinueTxn(opCtx(), txnNum, false, true, "admin", "commitTransaction");
    session.unstashTransactionResources(opCtx(), "commitTransaction");

    startCapturingLogMessages();
    session.commitTransaction(opCtx());
    stopCapturingLogMessages();

    // Test that the transaction is still logged.
    ASSERT_EQUALS(1, countLogLinesContaining("transaction parameters"));
}

}  // namespace
}  // namespace mongo
