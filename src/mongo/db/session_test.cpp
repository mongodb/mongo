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

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class SessionTest : public MockReplCoordServerFixture {
public:
    void setUp() override {
        MockReplCoordServerFixture::setUp();

        auto service = opCtx()->getServiceContext();
        SessionCatalog::reset_forTest(service);
        SessionCatalog::create(service);
        SessionCatalog::get(service)->onStepUp(opCtx());
    }
};

TEST_F(SessionTest, CanCreateNewSessionEntry) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_TRUE(txnState.getLastWriteOpTimeTs().isNull());

    DBDirectClient client(opCtx());
    Query queryAll;
    queryAll.sort(BSON("_id" << 1));

    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    auto doc = cursor->next();
    IDLParserErrorContext ctx1("CanCreateNewSessionEntry");
    auto txnRecord = SessionTxnRecord::parse(ctx1, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_TRUE(txnRecord.getLastWriteOpTimeTs().isNull());

    ASSERT_FALSE(cursor->more());
}

TEST_F(SessionTest, StartingOldTxnShouldAssert) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    ASSERT_THROWS(txnState.begin(opCtx(), txnNum - 1), AssertionException);
    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_TRUE(txnState.getLastWriteOpTimeTs().isNull());
}

TEST_F(SessionTest, StartingNewSessionWithCompatibleEntryInStorage) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    const Timestamp origTs(985, 15);

    SessionTxnRecord origRecord;
    origRecord.setSessionId(sessionId);
    origRecord.setTxnNum(txnNum);
    origRecord.setLastWriteOpTimeTs(origTs);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), origRecord.toBSON());

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_EQ(origTs, txnState.getLastWriteOpTimeTs());

    // Confirm that nothing changed in storage.

    Query queryAll;
    queryAll.sort(BSON("_id" << 1));
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    auto doc = cursor->next();
    IDLParserErrorContext ctx1("StartingNewSessionWithCompatibleEntryInStorage");
    auto txnRecord = SessionTxnRecord::parse(ctx1, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(origTs, txnRecord.getLastWriteOpTimeTs());

    ASSERT_FALSE(cursor->more());
}

TEST_F(SessionTest, StartingNewSessionWithOlderEntryInStorageShouldUpdateEntry) {
    const auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 20;
    const Timestamp origTs(985, 15);

    SessionTxnRecord origRecord;
    origRecord.setSessionId(sessionId);
    origRecord.setTxnNum(txnNum);
    origRecord.setLastWriteOpTimeTs(origTs);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), origRecord.toBSON());

    Session txnState(sessionId);
    txnState.begin(opCtx(), ++txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_TRUE(txnState.getLastWriteOpTimeTs().isNull());

    // Confirm that entry has new txn and ts reset to zero in storage.

    Query queryAll;
    queryAll.sort(BSON("_id" << 1));
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    auto doc = cursor->next();
    IDLParserErrorContext ctx1("StartingNewSessionWithOlderEntryInStorageShouldUpdateEntry");
    auto txnRecord = SessionTxnRecord::parse(ctx1, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_TRUE(txnRecord.getLastWriteOpTimeTs().isNull());

    ASSERT_FALSE(cursor->more());
}

TEST_F(SessionTest, StartingNewSessionWithNewerEntryInStorageShouldAssert) {
    const auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 20;
    const Timestamp origTs(985, 15);

    SessionTxnRecord origRecord;
    origRecord.setSessionId(sessionId);
    origRecord.setTxnNum(txnNum);
    origRecord.setLastWriteOpTimeTs(origTs);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), origRecord.toBSON());

    Session txnState(sessionId);
    ASSERT_THROWS(txnState.begin(opCtx(), txnNum - 1), AssertionException);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_EQ(origTs, txnState.getLastWriteOpTimeTs());

    // Confirm that nothing changed in storage.

    Query queryAll;
    queryAll.sort(BSON("_id" << 1));
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    auto doc = cursor->next();
    IDLParserErrorContext ctx1("StartingNewSessionWithOlderEntryInStorageShouldUpdateEntry");
    auto txnRecord = SessionTxnRecord::parse(ctx1, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(origTs, txnRecord.getLastWriteOpTimeTs());

    ASSERT_FALSE(cursor->more());
}

TEST_F(SessionTest, StoreOpTime) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    const Timestamp ts1(100, 42);

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    {
        AutoGetCollection autoColl(opCtx(), NamespaceString("test.user"), MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        txnState.saveTxnProgress(opCtx(), ts1);
        wuow.commit();
    }

    DBDirectClient client(opCtx());
    Query queryAll;
    queryAll.sort(BSON("_id" << 1));

    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    auto doc = cursor->next();
    IDLParserErrorContext ctx1("StoreOpTime 1");
    auto txnRecord = SessionTxnRecord::parse(ctx1, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(ts1, txnRecord.getLastWriteOpTimeTs());

    ASSERT_FALSE(cursor->more());

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_EQ(ts1, txnState.getLastWriteOpTimeTs());

    const Timestamp ts2(200, 23);
    {
        AutoGetCollection autoColl(opCtx(), NamespaceString("test.user"), MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        txnState.saveTxnProgress(opCtx(), ts2);
        wuow.commit();
    }

    cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    doc = cursor->next();
    IDLParserErrorContext ctx2("StoreOpTime 2");
    txnRecord = SessionTxnRecord::parse(ctx2, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(ts2, txnRecord.getLastWriteOpTimeTs());

    ASSERT_FALSE(cursor->more());

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_EQ(ts2, txnState.getLastWriteOpTimeTs());
}

TEST_F(SessionTest, CanBumpTransactionIdIfNewer) {
    const auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 20;
    const Timestamp ts1(100, 42);

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    {
        AutoGetCollection autoColl(opCtx(), NamespaceString("test.user"), MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        txnState.saveTxnProgress(opCtx(), ts1);
        wuow.commit();
    }

    DBDirectClient client(opCtx());
    Query queryAll;
    queryAll.sort(BSON("_id" << 1));

    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    auto doc = cursor->next();
    IDLParserErrorContext ctx1("CanBumpTransactionIdIfNewer 1");
    auto txnRecord = SessionTxnRecord::parse(ctx1, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(ts1, txnRecord.getLastWriteOpTimeTs());

    ASSERT_FALSE(cursor->more());

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_EQ(ts1, txnState.getLastWriteOpTimeTs());

    // Start a new transaction on the same session.
    txnState.begin(opCtx(), ++txnNum);

    cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());
    doc = cursor->next();
    IDLParserErrorContext ctx2("CanBumpTransactionIdIfNewer 2");
    txnRecord = SessionTxnRecord::parse(ctx2, doc);

    ASSERT_EQ(sessionId, txnRecord.getSessionId());
    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_TRUE(txnRecord.getLastWriteOpTimeTs().isNull());

    ASSERT_FALSE(cursor->more());

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT_TRUE(txnState.getLastWriteOpTimeTs().isNull());
}

TEST_F(SessionTest, StartingNewSessionWithDroppedTableShouldAssert) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;

    const auto& ns = NamespaceString::kSessionTransactionsTableNamespace;

    BSONObj dropResult;
    DBDirectClient client(opCtx());
    ASSERT_TRUE(client.runCommand(ns.db().toString(), BSON("drop" << ns.coll()), dropResult));

    Session txnState(sessionId);
    ASSERT_THROWS(txnState.begin(opCtx(), txnNum), AssertionException);

    ASSERT_EQ(sessionId, txnState.getSessionId());
}

TEST_F(SessionTest, SaveTxnProgressShouldAssertIfTableIsDropped) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    const Timestamp ts1(100, 42);

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    const auto& ns = NamespaceString::kSessionTransactionsTableNamespace;

    BSONObj dropResult;
    DBDirectClient client(opCtx());
    ASSERT_TRUE(client.runCommand(ns.db().toString(), BSON("drop" << ns.coll()), dropResult));

    AutoGetCollection autoColl(opCtx(), NamespaceString("test.user"), MODE_IX);
    WriteUnitOfWork wuow(opCtx());

    ASSERT_THROWS(txnState.saveTxnProgress(opCtx(), ts1), AssertionException);
}

TEST_F(SessionTest, TwoSessionsShouldBeIndependent) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const TxnNumber txnNum1 = 20;
    const Timestamp ts1(1903, 42);

    Session txnState1(sessionId1);
    txnState1.begin(opCtx(), txnNum1);

    const auto sessionId2 = makeLogicalSessionIdForTest();
    const TxnNumber txnNum2 = 300;
    const Timestamp ts2(671, 5);

    Session txnState2(sessionId2);
    txnState2.begin(opCtx(), txnNum2);

    {
        AutoGetCollection autoColl(opCtx(), NamespaceString("test.user"), MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        txnState2.saveTxnProgress(opCtx(), ts2);
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx(), NamespaceString("test.user"), MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        txnState1.saveTxnProgress(opCtx(), ts1);
        wuow.commit();
    }

    DBDirectClient client(opCtx());
    Query queryAll;
    queryAll.sort(BSON(SessionTxnRecord::kTxnNumFieldName << 1));

    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), queryAll);
    ASSERT_TRUE(cursor.get() != nullptr);

    ASSERT_TRUE(cursor->more());

    {
        auto doc = cursor->next();
        IDLParserErrorContext ctx("TwoSessionsShouldBeIndependent 1");
        auto txnRecord = SessionTxnRecord::parse(ctx, doc);

        ASSERT_EQ(sessionId1, txnRecord.getSessionId());
        ASSERT_EQ(txnNum1, txnRecord.getTxnNum());
        ASSERT_EQ(ts1, txnRecord.getLastWriteOpTimeTs());
    }

    ASSERT_TRUE(cursor->more());

    {
        auto doc = cursor->next();
        IDLParserErrorContext ctx("TwoSessionsShouldBeIndependent 2");
        auto txnRecord = SessionTxnRecord::parse(ctx, doc);

        ASSERT_EQ(sessionId2, txnRecord.getSessionId());
        ASSERT_EQ(txnNum2, txnRecord.getTxnNum());
        ASSERT_EQ(ts2, txnRecord.getLastWriteOpTimeTs());
    }

    ASSERT_FALSE(cursor->more());
}

TEST_F(SessionTest, CheckStatementExecuted) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;
    const StmtId stmtId = 5;

    opCtx()->setLogicalSessionId(sessionId);
    opCtx()->setTxnNumber(txnNum);

    Session session(sessionId);
    session.begin(opCtx(), txnNum);

    // Returns nothing if the statement has not been executed.
    auto fetchedEntry = session.checkStatementExecuted(opCtx(), stmtId);
    ASSERT_FALSE(fetchedEntry);

    // Returns the correct oplog entry if the statement has completed.
    auto optimeTs = Timestamp(50, 10);

    OperationSessionInfo opSessionInfo;
    opSessionInfo.setSessionId(sessionId);
    opSessionInfo.setTxnNumber(txnNum);

    repl::OplogEntry oplogEntry(repl::OpTime(optimeTs, 1),
                                0,
                                repl::OpTypeEnum::kInsert,
                                NamespaceString("a.b"),
                                0,
                                BSON("_id" << 1 << "x" << 5));
    oplogEntry.setOperationSessionInfo(opSessionInfo);
    oplogEntry.setStatementId(stmtId);
    oplogEntry.setPrevWriteTsInTransaction(Timestamp(0, 0));
    insertOplogEntry(oplogEntry);

    {
        AutoGetCollection autoColl(opCtx(), NamespaceString("a.b"), MODE_IX);
        WriteUnitOfWork wuow(opCtx());

        session.saveTxnProgress(opCtx(), optimeTs);
        wuow.commit();
    }

    fetchedEntry = session.checkStatementExecuted(opCtx(), stmtId);
    ASSERT_TRUE(fetchedEntry);
    ASSERT_EQ(fetchedEntry->getStatementId().get(), stmtId);

    // Still returns nothing for uncompleted statements.
    auto uncompletedStmtId = 10;
    fetchedEntry = session.checkStatementExecuted(opCtx(), uncompletedStmtId);
    ASSERT_FALSE(fetchedEntry);
}

TEST_F(SessionTest, BeginReloadsStateAfterReset) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT(txnState.getLastWriteOpTimeTs().isNull());

    const TxnNumber newTxnNum = 50;
    const auto newTs = Timestamp(1, 1);
    Session::updateSessionRecord(opCtx(), sessionId, newTxnNum, newTs);
    txnState.reset();

    txnState.begin(opCtx(), newTxnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(newTxnNum, txnState.getTxnNum());
    ASSERT_EQ(txnState.getLastWriteOpTimeTs(), newTs);
}

TEST_F(SessionTest, BeginDoesNotReloadWithoutReset) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT(txnState.getLastWriteOpTimeTs().isNull());

    const TxnNumber newTxnNum = 30;
    const auto newTs = Timestamp(1, 1);
    Session::updateSessionRecord(opCtx(), sessionId, newTxnNum, newTs);

    txnState.begin(opCtx(), txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT(txnState.getLastWriteOpTimeTs().isNull());
}

TEST_F(SessionTest, StartingOldTxnFailsAfterReset) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber oldTxnNum = 20;

    Session txnState(sessionId);
    txnState.begin(opCtx(), oldTxnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(oldTxnNum, txnState.getTxnNum());
    ASSERT(txnState.getLastWriteOpTimeTs().isNull());

    const TxnNumber newTxnNum = 30;
    Session::updateSessionRecord(opCtx(), sessionId, newTxnNum, Timestamp());
    txnState.reset();

    ASSERT_THROWS(txnState.begin(opCtx(), oldTxnNum), AssertionException);
}

TEST_F(SessionTest, CanStartLaterTxnAfterReset) {
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 20;

    Session txnState(sessionId);
    txnState.begin(opCtx(), txnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(txnNum, txnState.getTxnNum());
    ASSERT(txnState.getLastWriteOpTimeTs().isNull());

    txnState.reset();

    const TxnNumber newTxnNum = 40;
    txnState.begin(opCtx(), newTxnNum);

    ASSERT_EQ(sessionId, txnState.getSessionId());
    ASSERT_EQ(newTxnNum, txnState.getTxnNum());
    ASSERT(txnState.getLastWriteOpTimeTs().isNull());
}

}  // namespace
}  // namespace mongo
