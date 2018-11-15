
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

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

using repl::OplogEntry;
using unittest::assertGet;

namespace {

class OpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            stdx::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::setOplogCollectionName(service);
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

protected:
    // Assert that oplog only has a single entry and return that oplog entry.
    BSONObj getSingleOplogEntry(OperationContext* opCtx) {
        repl::OplogInterfaceLocal oplogInterface(opCtx, NamespaceString::kRsOplogNamespace.ns());
        auto oplogIter = oplogInterface.makeIterator();
        auto opEntry = unittest::assertGet(oplogIter->next());
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
        return opEntry.first;
    }

private:
    // Creates a reasonable set of ReplSettings for most tests.  We need to be able to
    // override this to create a larger oplog.
    virtual repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(OpObserverTest, CollModWithCollectionOptionsAndTTLInfo) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'collMod' command.
    NamespaceString nss("test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn"
                                        // We verify that 'onCollMod' ignores this field.
                                        << "index"
                                        << "indexData");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = "strict";
    oldCollOpts.validationAction = "error";
    oldCollOpts.flags = 2;
    oldCollOpts.flagsSet = true;

    TTLCollModInfo ttlInfo;
    ttlInfo.expireAfterSeconds = Seconds(10);
    ttlInfo.oldExpireAfterSeconds = Seconds(5);
    ttlInfo.indexName = "name_of_index";

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, ttlInfo);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to the oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected =
        BSON("collMod" << nss.coll() << "validationLevel"
                       << "off"
                       << "validationAction"
                       << "warn"
                       << "index"
                       << BSON("name" << ttlInfo.indexName << "expireAfterSeconds"
                                      << durationCount<Seconds>(ttlInfo.expireAfterSeconds)));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected =
        BSON("collectionOptions_old" << BSON("flags" << oldCollOpts.flags << "validationLevel"
                                                     << oldCollOpts.validationLevel
                                                     << "validationAction"
                                                     << oldCollOpts.validationAction)
                                     << "expireAfterSeconds_old"
                                     << durationCount<Seconds>(ttlInfo.oldExpireAfterSeconds));

    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, CollModWithOnlyCollectionOptions) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'collMod' command.
    NamespaceString nss("test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = "strict";
    oldCollOpts.validationAction = "error";

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, boost::none);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = collModCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved and that TTL info is not present.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected =
        BSON("collectionOptions_old"
             << BSON("validationLevel" << oldCollOpts.validationLevel << "validationAction"
                                       << oldCollOpts.validationAction));
    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, OnDropCollectionReturnsDropOpTime) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'drop' command.
    NamespaceString nss("test.coll");
    auto dropCmd = BSON("drop" << nss.coll());

    // Write to the oplog.
    repl::OpTime dropOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onDropCollection(
            opCtx.get(), nss, uuid, OpObserver::CollectionDropType::kTwoPhase);
        dropOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that drop fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = dropCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the drop optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), dropOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionReturnsRenameOpTime) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto uuid = CollectionUUID::gen();
    auto dropTargetUuid = CollectionUUID::gen();
    auto stayTemp = false;
    NamespaceString sourceNss("test.foo");
    NamespaceString targetNss("test.bar");

    // Write to the oplog.
    repl::OpTime renameOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(
            opCtx.get(), sourceNss, targetNss, uuid, dropTargetUuid, stayTemp);
        renameOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "renameCollection" << sourceNss.ns() << "to" << targetNss.ns() << "stayTemp" << stayTemp
                           << "dropTarget"
                           << dropTargetUuid);
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the rename optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), renameOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionOmitsDropTargetFieldIfDropTargetUuidIsNull) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto uuid = CollectionUUID::gen();
    auto stayTemp = true;
    NamespaceString sourceNss("test.foo");
    NamespaceString targetNss("test.bar");

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(opCtx.get(), sourceNss, targetNss, uuid, {}, stayTemp);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "renameCollection" << sourceNss.ns() << "to" << targetNss.ns() << "stayTemp" << stayTemp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
}

/**
 * Test fixture for testing OpObserver behavior specific to the SessionCatalog.
 */
class OpObserverSessionCatalogRollbackTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();

        auto opCtx = cc().makeOperationContext();
        MongoDSessionCatalog::onStepUp(opCtx.get());
    }

    /**
     * Simulate a new write occurring on given session with the given transaction number and
     * statement id.
     */
    void simulateSessionWrite(OperationContext* opCtx,
                              TransactionParticipant* txnParticipant,
                              NamespaceString nss,
                              TxnNumber txnNum,
                              StmtId stmtId) {
        txnParticipant->beginOrContinue(txnNum, boost::none, boost::none);

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            auto opTime = repl::OpTime(Timestamp(10, 1), 1);  // Dummy timestamp.
            txnParticipant->onWriteOpCompletedOnPrimary(
                opCtx, txnNum, {stmtId}, opTime, Date_t::now(), boost::none);
            wuow.commit();
        }
    }
};

TEST_F(OpObserverSessionCatalogRollbackTest,
       OnRollbackInvalidatesSessionCatalogIfSessionOpsRolledBack) {
    const NamespaceString nss("testDB", "testColl");

    // Create a session.
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    auto sessionId = makeLogicalSessionIdForTest();

    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);

        // Create a session and sync it from disk
        auto session = sessionCatalog->checkOutSession(opCtx.get());
        const auto txnParticipant =
            TransactionParticipant::getFromNonCheckedOutSession(session.get());
        txnParticipant->refreshFromStorageIfNeeded(opCtx.get());

        // Simulate a write occurring on that session
        simulateSessionWrite(opCtx.get(), txnParticipant, nss, txnNum, stmtId);

        // Check that the statement executed
        ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId));
    }

    // The OpObserver should invalidate in-memory session state, so the check after this should
    // fail.
    {
        auto opCtx = cc().makeOperationContext();

        OpObserverImpl opObserver;
        OpObserver::RollbackObserverInfo rbInfo;
        rbInfo.rollbackSessionIds = {UUID::gen()};
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);

        auto session = sessionCatalog->checkOutSession(opCtx.get());
        const auto txnParticipant =
            TransactionParticipant::getFromNonCheckedOutSession(session.get());
        ASSERT_THROWS_CODE(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId),
                           DBException,
                           ErrorCodes::ConflictingOperationInProgress);
    }
}

TEST_F(OpObserverSessionCatalogRollbackTest,
       OnRollbackDoesntInvalidateSessionCatalogIfNoSessionOpsRolledBack) {
    const NamespaceString nss("testDB", "testColl");

    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    auto sessionId = makeLogicalSessionIdForTest();

    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);

        // Create a session and sync it from disk
        auto session = sessionCatalog->checkOutSession(opCtx.get());
        const auto txnParticipant =
            TransactionParticipant::getFromNonCheckedOutSession(session.get());
        txnParticipant->refreshFromStorageIfNeeded(opCtx.get());

        // Simulate a write occurring on that session
        simulateSessionWrite(opCtx.get(), txnParticipant, nss, txnNum, stmtId);

        // Check that the statement executed
        ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId));
    }

    // Because there are no sessions to rollback, the OpObserver should not invalidate the in-memory
    // session state, so the check after this should still succeed.
    {
        auto opCtx = cc().makeOperationContext();

        OpObserverImpl opObserver;
        OpObserver::RollbackObserverInfo rbInfo;
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);

        auto session = sessionCatalog->checkOutSession(opCtx.get());
        const auto txnParticipant =
            TransactionParticipant::getFromNonCheckedOutSession(session.get());
        ASSERT(txnParticipant->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId));
    }
}

TEST_F(OpObserverTest, OnRollbackInvalidatesAuthCacheWhenAuthNamespaceRolledBack) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto authMgr = AuthorizationManager::get(getServiceContext());
    auto initCacheGen = authMgr->getCacheGeneration();

    // Verify that the rollback op observer invalidates the user cache for each auth namespace by
    // checking that the cache generation changes after a call to the rollback observer method.
    auto nss = AuthorizationManager::rolesCollectionNamespace;
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.rollbackNamespaces = {AuthorizationManager::rolesCollectionNamespace};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_NE(initCacheGen, authMgr->getCacheGeneration());

    initCacheGen = authMgr->getCacheGeneration();
    rbInfo.rollbackNamespaces = {AuthorizationManager::usersCollectionNamespace};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_NE(initCacheGen, authMgr->getCacheGeneration());

    initCacheGen = authMgr->getCacheGeneration();
    rbInfo.rollbackNamespaces = {AuthorizationManager::versionCollectionNamespace};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_NE(initCacheGen, authMgr->getCacheGeneration());
}

TEST_F(OpObserverTest, OnRollbackDoesntInvalidateAuthCacheWhenNoAuthNamespaceRolledBack) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto authMgr = AuthorizationManager::get(getServiceContext());
    auto initCacheGen = authMgr->getCacheGeneration();

    // Verify that the rollback op observer doesn't invalidate the user cache.
    auto nss = AuthorizationManager::rolesCollectionNamespace;
    OpObserver::RollbackObserverInfo rbInfo;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    auto newCacheGen = authMgr->getCacheGeneration();
    ASSERT_EQ(newCacheGen, initCacheGen);
}

TEST_F(OpObserverTest, MultipleAboutToDeleteAndOnDelete) {
    auto uuid = UUID::gen();
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss = {"test", "coll"};
    AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
    WriteUnitOfWork wunit(opCtx.get());
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, uuid, {}, false, {});
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, uuid, {}, false, {});
}

DEATH_TEST_F(OpObserverTest, AboutToDeleteMustPreceedOnDelete, "invariant") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    opCtx->swapLockState(stdx::make_unique<LockerNoop>());
    NamespaceString nss = {"test", "coll"};
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
}

DEATH_TEST_F(OpObserverTest, EachOnDeleteRequiresAboutToDelete, "invariant") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    opCtx->swapLockState(stdx::make_unique<LockerNoop>());
    NamespaceString nss = {"test", "coll"};
    opObserver.aboutToDelete(opCtx.get(), nss, {});
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
}

DEATH_TEST_F(OpObserverTest,
             NodeCrashesIfShardIdentityDocumentRolledBack,
             "Fatal Assertion 50712") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.shardIdentityRolledBack = true;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
}

/**
 * Test fixture for testing OpObserver behavior specific to multi-document transactions.
 */

class OpObserverTransactionTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();
        _opCtx = cc().makeOperationContext();
        _opObserver.emplace();

        MongoDSessionCatalog::onStepUp(opCtx());

        // Create a session.
        auto sessionCatalog = SessionCatalog::get(getServiceContext());
        auto sessionId = makeLogicalSessionIdForTest();
        _session = sessionCatalog->getOrCreateSession(opCtx(), sessionId);

        _times.emplace(opCtx());
        opCtx()->setLogicalSessionId(session()->getSessionId());
        opCtx()->setTxnNumber(txnNum());

        _sessionCheckout = std::make_unique<MongoDOperationContextSession>(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant->beginOrContinue(*opCtx()->getTxnNumber(), false, true);
    }

    void tearDown() override {
        _sessionCheckout.reset();
        _times.reset();
        _opCtx.reset();

        OpObserverTest::tearDown();
    }


protected:
    void checkCommonFields(const BSONObj& oplogEntry, int expectedStmtId = 0) {
        ASSERT_EQ("c"_sd, oplogEntry.getStringField("op"));
        ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getStringField("ns"));
        ASSERT_BSONOBJ_EQ(session()->getSessionId().toBSON(), oplogEntry.getObjectField("lsid"));
        ASSERT_EQ(*opCtx()->getTxnNumber(), oplogEntry.getField("txnNumber").safeNumberLong());
        ASSERT_EQ(expectedStmtId, oplogEntry.getIntField("stmtId"));
    }

    void assertTxnRecord(TxnNumber txnNum,
                         repl::OpTime opTime,
                         boost::optional<DurableTxnStateEnum> txnState) {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord =
            SessionTxnRecord::parse(IDLParserErrorContext("SessionEntryWritten"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session()->getSessionId(), txnRecord.getSessionId());
        ASSERT_EQ(txnNum, txnRecord.getTxnNum());
        ASSERT(txnRecord.getState() == txnState);
        ASSERT_EQ(txnState != boost::none,
                  txnRecordObj.hasField(SessionTxnRecord::kStateFieldName));

        const auto txnParticipant = TransactionParticipant::getFromNonCheckedOutSession(session());
        if (!opTime.isNull()) {
            ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
            ASSERT_EQ(opTime, txnParticipant->getLastWriteOpTime(txnNum));
        } else {
            ASSERT_EQ(txnRecord.getLastWriteOpTime(), txnParticipant->getLastWriteOpTime(txnNum));
        }
    }

    void assertNoTxnRecord() {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(!cursor->more());
    }

    Session* session() {
        return _session->get();
    }

    OpObserverImpl& opObserver() {
        return *_opObserver;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    TxnNumber& txnNum() {
        return _txnNum;
    }

private:
    class ExposeOpObserverTimes : public OpObserver {
    public:
        typedef OpObserver::ReservedTimes ReservedTimes;
    };

    boost::optional<OpObserverImpl> _opObserver;
    boost::optional<ScopedSession> _session;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::optional<ExposeOpObserverTimes::ReservedTimes> _times;
    std::unique_ptr<MongoDOperationContextSession> _sessionCheckout;
    TxnNumber _txnNum = 0;
};

/**
 * Test fixture with sessions and an extra-large oplog for testing large transactions.
 */
class OpObserverLargeTransactionTest : public OpObserverTransactionTest {
private:
    repl::ReplSettings createReplSettings() override {
        repl::ReplSettings settings;
        // We need an oplog comfortably large enough to hold an oplog entry that exceeds the BSON
        // size limit.  Otherwise we will get the wrong error code when trying to write one.
        settings.setOplogSizeBytes(BSONObjMaxInternalSize + 2 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

// Tests that a transaction aborts if it becomes too large only during the commit.
TEST_F(OpObserverLargeTransactionTest, TransactionTooLargeWhileCommitting) {
    const NamespaceString nss("testDB", "testColl");
    auto uuid = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    // This size is crafted such that two operations of this size are not too big to fit in a single
    // oplog entry, but two operations plus oplog overhead are too big to fit in a single oplog
    // entry.
    constexpr size_t kHalfTransactionSize = BSONObjMaxInternalSize / 2 - 175;
    std::unique_ptr<uint8_t[]> halfTransactionData(new uint8_t[kHalfTransactionSize]());
    auto operation = repl::OplogEntry::makeInsertOperation(
        nss,
        uuid,
        BSON(
            "_id" << 0 << "data"
                  << BSONBinData(halfTransactionData.get(), kHalfTransactionSize, BinDataGeneral)));
    txnParticipant->addTransactionOperation(opCtx(), operation);
    txnParticipant->addTransactionOperation(opCtx(), operation);
    ASSERT_THROWS_CODE(opObserver().onTransactionCommit(opCtx(), boost::none, boost::none),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
}

TEST_F(OpObserverTransactionTest, TransactionalPrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtId = 1;
    updateArgs2.updatedDoc = BSON("_id" << 0 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);
    opObserver().onUpdate(opCtx(), update2);

    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, false, boost::none);

    txnParticipant->transitionToPreparedforTest();
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        opObserver().onTransactionPrepare(opCtx(), slot);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("_id" << 0 << "data"
                                                                      << "x"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss1.toString()
                                                           << "ui"
                                                           << uuid1
                                                           << "o"
                                                           << BSON("_id" << 1 << "data"
                                                                         << "y"))
                                                   << BSON("op"
                                                           << "u"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("$set" << BSON("data"
                                                                                  << "y"))
                                                           << "o2"
                                                           << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "d"
                                                           << "ns"
                                                           << nss1.toString()
                                                           << "ui"
                                                           << uuid1
                                                           << "o"
                                                           << BSON("_id" << 0)))
                                     << "prepare"
                                     << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.getPrepare());
    ASSERT(oplogEntry.getPrepare().get());
    ASSERT_EQ(oplogEntry.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedCommitTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot commitSlot;
    Timestamp prepareTimestamp;
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        txnParticipant->transitionToPreparedforTest();
        const auto prepareSlot = repl::getNextOpTime(opCtx());
        prepareTimestamp = prepareSlot.opTime.getTimestamp();
        opObserver().onTransactionPrepare(opCtx(), prepareSlot);

        commitSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    opObserver().onTransactionCommit(opCtx(), commitSlot, prepareTimestamp);

    repl::OplogInterfaceLocal oplogInterface(opCtx(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj, 1);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << prepareTimestamp);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.getPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                            << "i"
                                                            << "ns"
                                                            << nss.toString()
                                                            << "ui"
                                                            << uuid
                                                            << "o"
                                                            << doc))
                                         << "prepare"
                                         << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.getPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedAbortTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot abortSlot;
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        txnParticipant->transitionToPreparedforTest();
        const auto prepareSlot = repl::getNextOpTime(opCtx());
        opObserver().onTransactionPrepare(opCtx(), prepareSlot);
        abortSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic aborting the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    opObserver().onTransactionAbort(opCtx(), abortSlot);
    txnParticipant->transitionToAbortedforTest();

    repl::OplogInterfaceLocal oplogInterface(opCtx(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj, 1);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("abortTransaction" << 1);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.getPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                            << "i"
                                                            << "ns"
                                                            << nss.toString()
                                                            << "ui"
                                                            << uuid
                                                            << "o"
                                                            << doc))
                                         << "prepare"
                                         << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.getPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalUnpreparedAbortTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        txnParticipant->transitionToAbortedforTest();
        opObserver().onTransactionAbort(opCtx(), boost::none);
    }

    // Assert no oplog entries were written.
    repl::OplogInterfaceLocal oplogInterface(opCtx(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, PreparingEmptyTransactionLogsEmptyApplyOps) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->transitionToPreparedforTest();

    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        opObserver().onTransactionPrepare(opCtx(), slot);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSONArray() << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.getPrepare());
    ASSERT(oplogEntry.getPrepare().get());
    ASSERT_EQ(oplogEntry.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
}

TEST_F(OpObserverTransactionTest, PreparingTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant->transitionToPreparedforTest();

    repl::OpTime prepareOpTime;
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        prepareOpTime = slot.opTime;
        opObserver().onTransactionPrepare(opCtx(), slot);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
    }

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    txnParticipant->stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant->unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverTransactionTest, AbortingUnpreparedTransactionDoesNotWriteToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onTransactionAbort(opCtx(), boost::none);
    txnParticipant->stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant->shutdown();

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, AbortingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    OplogSlot abortSlot;
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        opObserver().onTransactionPrepare(opCtx(), slot);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
        txnParticipant->transitionToPreparedforTest();
        abortSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic aborting the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    opObserver().onTransactionAbort(opCtx(), abortSlot);
    txnParticipant->transitionToAbortedforTest();

    txnParticipant->stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant->shutdown();

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kAborted);
}

TEST_F(OpObserverTransactionTest, CommittingUnpreparedNonEmptyTransactionWritesToTransactionTable) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);
    }

    opObserver().onTransactionCommit(opCtx(), boost::none, boost::none);
    opCtx()->getWriteUnitOfWork()->commit();

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest,
       CommittingUnpreparedEmptyTransactionDoesNotWriteToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onTransactionCommit(opCtx(), boost::none, boost::none);

    txnParticipant->stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant->shutdown();

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, CommittingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        prepareOpTime = slot.opTime;
        opObserver().onTransactionPrepare(opCtx(), slot);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
        txnParticipant->transitionToPreparedforTest();
    }

    OplogSlot commitSlot = repl::getNextOpTime(opCtx());
    repl::OpTime commitOpTime = commitSlot.opTime;
    ASSERT_LTE(prepareOpTime, commitOpTime);

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    opObserver().onTransactionCommit(opCtx(), commitSlot, prepareOpTime.getTimestamp());

    assertTxnRecord(txnNum(), commitOpTime, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest, TransactionalInsertTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0,
                          BSON("_id" << 2 << "data"
                                     << "z"));
    inserts2.emplace_back(1,
                          BSON("_id" << 3 << "data"
                                     << "w"));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);
    opObserver().onTransactionCommit(opCtx(), boost::none, boost::none);
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("_id" << 0 << "data"
                                                                      << "x"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss1.toString()
                                                           << "ui"
                                                           << uuid1
                                                           << "o"
                                                           << BSON("_id" << 1 << "data"
                                                                         << "y"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("_id" << 2 << "data"
                                                                         << "z"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("_id" << 3 << "data"
                                                                         << "w"))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(!oplogEntry.getPrepare());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalUpdateTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtId = 0;
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtId = 1;
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    opObserver().onTransactionCommit(opCtx(), boost::none, boost::none);
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "u"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("$set" << BSON("data"
                                                                               << "x"))
                                                        << "o2"
                                                        << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "u"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("$set" << BSON("data"
                                                                                  << "y"))
                                                           << "o2"
                                                           << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalDeleteTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant->unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, false, boost::none);
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, false, boost::none);
    opObserver().onTransactionCommit(opCtx(), boost::none, boost::none);
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "d"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

}  // namespace
}  // namespace mongo
