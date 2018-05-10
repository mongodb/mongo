/**
*    Copyright (C) 2017 MongoDB Inc.
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


#include "mongo/db/op_observer_impl.h"
#include "keys_collection_client_sharded.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
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
        opObserver.onDropCollection(opCtx.get(), nss, uuid);
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
class OpObserverSessionCatalogTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();
        auto opCtx = cc().makeOperationContext();
        auto sessionCatalog = SessionCatalog::get(getServiceContext());
        sessionCatalog->reset_forTest();
        sessionCatalog->onStepUp(opCtx.get());
    }

    /**
     * Simulate a new write occurring on given session with the given transaction number and
     * statement id.
     */
    void simulateSessionWrite(OperationContext* opCtx,
                              ScopedSession session,
                              NamespaceString nss,
                              TxnNumber txnNum,
                              StmtId stmtId) {
        session->beginOrContinueTxn(opCtx, txnNum, boost::none, boost::none, "testDB", "insert");

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            auto opTime = repl::OpTime(Timestamp(10, 1), 1);  // Dummy timestamp.
            session->onWriteOpCompletedOnPrimary(opCtx, txnNum, {stmtId}, opTime, Date_t::now());
            wuow.commit();
        }
    }
};

TEST_F(OpObserverSessionCatalogTest, OnRollbackInvalidatesSessionCatalogIfSessionOpsRolledBack) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    const NamespaceString nss("testDB", "testColl");

    // Create a session.
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    auto sessionId = makeLogicalSessionIdForTest();
    auto session = sessionCatalog->getOrCreateSession(opCtx.get(), sessionId);

    // Simulate a write occurring on that session.
    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;
    simulateSessionWrite(opCtx.get(), session, nss, txnNum, stmtId);

    // Check that the statement executed.
    ASSERT(session->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId));

    // The OpObserver should invalidate in-memory session state, so the check after this should
    // fail.
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.rollbackSessionIds = {UUID::gen()};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_THROWS_CODE(session->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(OpObserverSessionCatalogTest,
       OnRollbackDoesntInvalidateSessionCatalogIfNoSessionOpsRolledBack) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    const NamespaceString nss("testDB", "testColl");

    // Create a session.
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    auto sessionId = makeLogicalSessionIdForTest();
    auto session = sessionCatalog->getOrCreateSession(opCtx.get(), sessionId);

    // Simulate a write occurring on that session.
    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;
    simulateSessionWrite(opCtx.get(), session, nss, txnNum, stmtId);

    // Check that the statement executed.
    ASSERT(session->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId));

    // The OpObserver should not invalidate the in-memory session state, so the check after this
    // should still succeed.
    OpObserver::RollbackObserverInfo rbInfo;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT(session->checkStatementExecutedNoOplogEntryFetch(txnNum, stmtId));
}

/**
 * Test fixture with sessions and an extra-large oplog for testing large transactions.
 */
class OpObserverLargeTransactionTest : public OpObserverSessionCatalogTest {
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
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    const NamespaceString nss("testDB", "testColl");

    // Create a session.
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    auto sessionId = makeLogicalSessionIdForTest();
    auto session = sessionCatalog->getOrCreateSession(opCtx.get(), sessionId);
    auto uuid = CollectionUUID::gen();

    // Simulate adding transaction data to a session.
    const TxnNumber txnNum = 0;
    opCtx->setLogicalSessionId(sessionId);
    opCtx->setTxnNumber(txnNum);
    OperationContextSession opSession(opCtx.get(),
                                      true /* checkOutSession */,
                                      false /* autocommit */,
                                      true /* startTransaction */,
                                      "testDB" /* dbName */,
                                      "insert" /* cmdName */);

    session->unstashTransactionResources(opCtx.get(), "insert");

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
    session->addTransactionOperation(opCtx.get(), operation);
    session->addTransactionOperation(opCtx.get(), operation);
    ASSERT_THROWS_CODE(opObserver.onTransactionCommit(opCtx.get()),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
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
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss = {"test", "coll"};
    AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
    WriteUnitOfWork wunit(opCtx.get());
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
}

/**
 * Test fixture for testing OpObserver behavior specific to multi-document transactions.
 */

class OpObserverTransactionTest : public OpObserverSessionCatalogTest {
public:
    void setUp() override {
        OpObserverSessionCatalogTest::setUp();

        // Create a session.
        _opCtx = cc().makeOperationContext();
        auto sessionCatalog = SessionCatalog::get(getServiceContext());
        auto sessionId = makeLogicalSessionIdForTest();
        _session = sessionCatalog->getOrCreateSession(opCtx(), sessionId);
        opCtx()->setLogicalSessionId(sessionId);
        _opObserver.emplace();
        _times.emplace(opCtx());
    }

    void tearDown() override {
        _times.reset();
        _opCtx.reset();
        OpObserverSessionCatalogTest::tearDown();
    }


protected:
    void checkCommonFields(const BSONObj& oplogEntry) {
        ASSERT_EQ("c"_sd, oplogEntry.getStringField("op"));
        ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getStringField("ns"));
        ASSERT_BSONOBJ_EQ(session()->getSessionId().toBSON(), oplogEntry.getObjectField("lsid"));
        ASSERT_EQ(*opCtx()->getTxnNumber(), oplogEntry.getField("txnNumber").safeNumberLong());
        ASSERT_EQ(0, oplogEntry.getIntField("stmtId"));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    Session* session() {
        return _session->get();
    }

    OpObserverImpl& opObserver() {
        return *_opObserver;
    }

private:
    class ExposeOpObserverTimes : public OpObserver {
    public:
        typedef OpObserver::ReservedTimes ReservedTimes;
    };

    ServiceContext::UniqueOperationContext _opCtx;
    boost::optional<OpObserverImpl> _opObserver;
    boost::optional<ExposeOpObserverTimes::ReservedTimes> _times;
    boost::optional<ScopedSession> _session;
};

TEST_F(OpObserverTransactionTest, TransactionalInsertTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    const TxnNumber txnNum = 2;
    opCtx()->setTxnNumber(txnNum);
    OperationContextSession opSession(opCtx(),
                                      true /* checkOutSession */,
                                      false /* autocommit */,
                                      true /* startTransaction*/,
                                      "testDB",
                                      "insert");

    session()->unstashTransactionResources(opCtx(), "insert");

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
    opObserver().onTransactionCommit(opCtx());
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
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
}

TEST_F(OpObserverTransactionTest, TransactionalUpdateTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    const TxnNumber txnNum = 3;
    opCtx()->setTxnNumber(txnNum);
    OperationContextSession opSession(opCtx(),
                                      true /* checkOutSession */,
                                      false /* autocommit */,
                                      true /* startTransaction*/,
                                      "testDB",
                                      "update");

    session()->unstashTransactionResources(opCtx(), "update");

    OplogUpdateEntryArgs update1;
    update1.nss = nss1;
    update1.uuid = uuid1;
    update1.stmtId = 0;
    update1.updatedDoc = BSON("_id" << 0 << "data"
                                    << "x");
    update1.update = BSON("$set" << BSON("data"
                                         << "x"));
    update1.criteria = BSON("_id" << 0);

    OplogUpdateEntryArgs update2;
    update2.nss = nss2;
    update2.uuid = uuid2;
    update2.stmtId = 1;
    update2.updatedDoc = BSON("_id" << 1 << "data"
                                    << "y");
    update2.update = BSON("$set" << BSON("data"
                                         << "y"));
    update2.criteria = BSON("_id" << 1);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    opObserver().onTransactionCommit(opCtx());
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
}

TEST_F(OpObserverTransactionTest, TransactionalDeleteTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    const TxnNumber txnNum = 3;
    opCtx()->setTxnNumber(txnNum);
    OperationContextSession opSession(opCtx(),
                                      true /* checkOutSession */,
                                      false /* autocommit */,
                                      true /* startTransaction*/,
                                      "testDB",
                                      "delete");

    session()->unstashTransactionResources(opCtx(), "delete");

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
    opObserver().onTransactionCommit(opCtx());
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

}  // namespace
}  // namespace mongo
