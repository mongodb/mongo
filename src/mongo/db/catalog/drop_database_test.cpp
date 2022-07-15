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

#include <memory>
#include <set>

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace {

using namespace mongo;

/**
 * Mock OpObserver that tracks dropped collections and databases.
 * Since this class is used exclusively to test dropDatabase(), we will also check the drop-pending
 * flag in the Database object being tested (if provided).
 */
class OpObserverMock : public OpObserverNoop {
public:
    void onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) override;

    using OpObserver::onDropCollection;
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType) override;

    std::set<std::string> droppedDatabaseNames;
    std::set<NamespaceString> droppedCollectionNames;
    Database* db = nullptr;
    bool onDropCollectionThrowsException = false;
    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

void OpObserverMock::onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onDropDatabase(opCtx, dbName);
    // Do not update 'droppedDatabaseNames' if OpObserverNoop::onDropDatabase() throws.
    // TODO: SERVER-67549 to change droppedDatabaseNames to use DatabaseName
    droppedDatabaseNames.insert(dbName.toStringWithTenantId());
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    auto opTime =
        OpObserverNoop::onDropCollection(opCtx, collectionName, uuid, numRecords, dropType);
    invariant(opTime.isNull());
    // Do not update 'droppedCollectionNames' if OpObserverNoop::onDropCollection() throws.
    droppedCollectionNames.insert(collectionName);

    // Check drop-pending flag in Database if provided.
    if (db) {
        ASSERT_TRUE(db->isDropPending(opCtx));
    }

    uassert(
        ErrorCodes::OperationFailed, "onDropCollection() failed", !onDropCollectionThrowsException);

    OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
    return {};
}

class DropDatabaseTest : public ServiceContextMongoDTest {
public:
    static ServiceContext::UniqueOperationContext makeOpCtx();

private:
    void setUp() override;
    void tearDown() override;

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    repl::ReplicationCoordinatorMock* _replCoord = nullptr;
    OpObserverMock* _opObserver = nullptr;
    NamespaceString _nss;
};

// static
ServiceContext::UniqueOperationContext DropDatabaseTest::makeOpCtx() {
    return cc().makeOperationContext();
}

void DropDatabaseTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
    repl::DropPendingCollectionReaper::set(
        service,
        std::make_unique<repl::DropPendingCollectionReaper>(repl::StorageInterface::get(service)));

    // Set up ReplicationCoordinator and create oplog.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    _replCoord = replCoord.get();
    repl::ReplicationCoordinator::set(service, std::move(replCoord));
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for collection and database drops.
    OpObserverRegistry* opObserverRegistry =
        dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
    auto mockObserver = std::make_unique<OpObserverMock>();
    _opObserver = mockObserver.get();
    opObserverRegistry->addObserver(std::move(mockObserver));

    _nss = NamespaceString("test.foo");
}

void DropDatabaseTest::tearDown() {
    _nss = {};
    _opObserver = nullptr;
    _replCoord = nullptr;
    _opCtx = {};

    auto service = getServiceContext();
    repl::DropPendingCollectionReaper::set(service, {});
    repl::StorageInterface::set(service, {});

    ServiceContextMongoDTest::tearDown();
}

/**
 * Creates a collection without any namespace restrictions.
 */
void _createCollection(OperationContext* opCtx, const NamespaceString& nss) {
    writeConflictRetry(opCtx, "testDropCollection", nss.ns(), [=] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_TRUE(db->createCollection(opCtx, nss));
        wuow.commit();
    });

    ASSERT_TRUE(AutoGetCollectionForRead(opCtx, nss).getCollection());
}

/**
 * Removes database from catalog, bypassing dropDatabase().
 */
void _removeDatabaseFromCatalog(OperationContext* opCtx, StringData dbName) {
    AutoGetDb autoDB(opCtx, dbName, MODE_X);
    auto db = autoDB.getDb();
    // dropDatabase can call awaitReplication more than once, so do not attempt to drop the database
    // twice.
    if (db) {
        auto databaseHolder = DatabaseHolder::get(opCtx);
        databaseHolder->dropDb(opCtx, db);
    }
}

TEST_F(DropDatabaseTest, DropDatabaseReturnsNamespaceNotFoundIfDatabaseDoesNotExist) {
    ASSERT_FALSE(AutoGetDb(_opCtx.get(), _nss.dbName(), MODE_X).getDb());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  dropDatabaseForApplyOps(_opCtx.get(), _nss.db().toString()));
}

TEST_F(DropDatabaseTest, DropDatabaseReturnsNotWritablePrimaryIfNotPrimary) {
    _createCollection(_opCtx.get(), _nss);
    ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(_replCoord->canAcceptWritesForDatabase(_opCtx.get(), _nss.db()));
    ASSERT_EQUALS(ErrorCodes::NotWritablePrimary,
                  dropDatabaseForApplyOps(_opCtx.get(), _nss.db().toString()));
}

/**
 * Tests successful drop of a database containing a single collection.
 * Checks expected number of onDropCollection() and onDropDatabase() invocations on the
 * OpObserver.
 * Checks that drop-pending flag is set by dropDatabase() during the collection drop phase.
 */
void _testDropDatabase(OperationContext* opCtx,
                       OpObserverMock* opObserver,
                       const NamespaceString& nss,
                       bool expectedOnDropCollection) {
    _createCollection(opCtx, nss);

    // Set OpObserverMock::db so that we can check Database::isDropPending() while dropping
    // collections.
    auto db = AutoGetDb(opCtx, nss.dbName(), MODE_X).getDb();
    ASSERT_TRUE(db);
    opObserver->db = db;

    ASSERT_OK(dropDatabaseForApplyOps(opCtx, nss.db().toString()));
    ASSERT_FALSE(AutoGetDb(opCtx, nss.dbName(), MODE_X).getDb());
    opObserver->db = nullptr;

    ASSERT_EQUALS(1U, opObserver->droppedDatabaseNames.size());
    ASSERT_EQUALS(nss.db().toString(), *(opObserver->droppedDatabaseNames.begin()));

    if (expectedOnDropCollection) {
        ASSERT_EQUALS(1U, opObserver->droppedCollectionNames.size());
        ASSERT_EQUALS(nss, *(opObserver->droppedCollectionNames.begin()));
    } else {
        ASSERT_EQUALS(0U, opObserver->droppedCollectionNames.size());
    }
}

TEST_F(DropDatabaseTest, DropDatabaseNotifiesOpObserverOfDroppedUserCollection) {
    _testDropDatabase(_opCtx.get(), _opObserver, _nss, true);
}

TEST_F(DropDatabaseTest, DropDatabaseNotifiesOpObserverOfDroppedReplicatedSystemCollection) {
    NamespaceString replicatedSystemNss(_nss.getSisterNS("system.js"));
    _testDropDatabase(_opCtx.get(), _opObserver, replicatedSystemNss, true);
}

TEST_F(DropDatabaseTest, DropDatabaseWaitsForDropPendingCollectionOpTimeIfNoCollectionsAreDropped) {
    repl::OpTime clientLastOpTime;

    // Update ReplicationCoordinatorMock so that we record the optime passed to awaitReplication().
    _replCoord->setAwaitReplicationReturnValueFunction(
        [&clientLastOpTime, this](OperationContext*, const repl::OpTime& opTime) {
            clientLastOpTime = opTime;
            ASSERT_GREATER_THAN(clientLastOpTime, repl::OpTime());
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    _testDropDatabase(_opCtx.get(), _opObserver, dpns, false);

    ASSERT_EQUALS(dropOpTime, clientLastOpTime);
}

TEST_F(DropDatabaseTest, DropDatabasePassedThroughAwaitReplicationErrorForDropPendingCollection) {
    // Update ReplicationCoordinatorMock so that we record the optime passed to awaitReplication().
    _replCoord->setAwaitReplicationReturnValueFunction(
        [this](OperationContext*, const repl::OpTime& opTime) {
            ASSERT_GREATER_THAN(opTime, repl::OpTime());
            return repl::ReplicationCoordinator::StatusAndDuration(
                Status(ErrorCodes::WriteConcernFailed, ""), Milliseconds(0));
        });

    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    _createCollection(_opCtx.get(), dpns);

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  dropDatabaseForApplyOps(_opCtx.get(), _nss.db().toString()));
}

TEST_F(DropDatabaseTest, DropDatabaseSkipsSystemProfileCollectionWhenDroppingCollections) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    NamespaceString profileNss(_nss.getSisterNS("system.profile"));
    _testDropDatabase(_opCtx.get(), _opObserver, profileNss, false);
}

TEST_F(DropDatabaseTest, DropDatabaseResetsDropPendingStateOnException) {
    // Update OpObserverMock so that onDropCollection() throws an exception when called.
    _opObserver->onDropCollectionThrowsException = true;

    _createCollection(_opCtx.get(), _nss);

    {
        AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);
    }

    ASSERT_THROWS_CODE_AND_WHAT(dropDatabaseForApplyOps(_opCtx.get(), _nss.db().toString()),
                                AssertionException,
                                ErrorCodes::OperationFailed,
                                "onDropCollection() failed");

    {
        AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_FALSE(db->isDropPending(_opCtx.get()));
    }
}

void _testDropDatabaseResetsDropPendingStateIfAwaitReplicationFails(OperationContext* opCtx,
                                                                    const NamespaceString& nss,
                                                                    bool expectDbPresent) {
    _createCollection(opCtx, nss);

    ASSERT_TRUE(AutoGetDb(opCtx, nss.dbName(), MODE_X).getDb());

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  dropDatabaseForApplyOps(opCtx, nss.db().toString()));

    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
    auto db = autoDb.getDb();
    if (expectDbPresent) {
        ASSERT_TRUE(db);
        ASSERT_FALSE(db->isDropPending(opCtx));
    } else {
        ASSERT_FALSE(db);
    }
}

TEST_F(DropDatabaseTest,
       DropDatabaseResetsDropPendingStateIfAwaitReplicationFailsAndDatabaseIsPresent) {
    // Update ReplicationCoordinatorMock so that awaitReplication() fails.
    _replCoord->setAwaitReplicationReturnValueFunction([](OperationContext*, const repl::OpTime&) {
        return repl::ReplicationCoordinator::StatusAndDuration(
            Status(ErrorCodes::WriteConcernFailed, ""), Milliseconds(0));
    });

    _testDropDatabaseResetsDropPendingStateIfAwaitReplicationFails(_opCtx.get(), _nss, true);
}

TEST_F(DropDatabaseTest,
       DropDatabaseResetsDropPendingStateIfAwaitReplicationFailsAndDatabaseIsMissing) {
    // Update ReplicationCoordinatorMock so that awaitReplication() fails.
    _replCoord->setAwaitReplicationReturnValueFunction(
        [this](OperationContext*, const repl::OpTime&) {
            _removeDatabaseFromCatalog(_opCtx.get(), _nss.db());
            return repl::ReplicationCoordinator::StatusAndDuration(
                Status(ErrorCodes::WriteConcernFailed, ""), Milliseconds(0));
        });

    _testDropDatabaseResetsDropPendingStateIfAwaitReplicationFails(_opCtx.get(), _nss, false);
}

TEST_F(DropDatabaseTest,
       DropDatabaseReturnsNamespaceNotFoundIfDatabaseIsRemovedAfterCollectionsDropsAreReplicated) {
    // Update ReplicationCoordinatorMock so that awaitReplication() fails.
    _replCoord->setAwaitReplicationReturnValueFunction(
        [this](OperationContext*, const repl::OpTime&) {
            _removeDatabaseFromCatalog(_opCtx.get(), _nss.db());
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    _createCollection(_opCtx.get(), _nss);

    ASSERT_TRUE(AutoGetDb(_opCtx.get(), _nss.dbName(), MODE_X).getDb());

    auto status = dropDatabaseForApplyOps(_opCtx.get(), _nss.db().toString());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
    ASSERT_EQUALS(status.reason(),
                  std::string(str::stream()
                              << "Could not drop database " << _nss.db()
                              << " because it does not exist after dropping 1 collection(s)."));

    ASSERT_FALSE(AutoGetDb(_opCtx.get(), _nss.dbName(), MODE_X).getDb());
}

TEST_F(DropDatabaseTest,
       DropDatabaseReturnsNotMasterIfNotPrimaryAfterCollectionsDropsAreReplicated) {
    // Transition from PRIMARY to SECONDARY while awaiting replication of collection drops.
    _replCoord->setAwaitReplicationReturnValueFunction(
        [this](OperationContext*, const repl::OpTime&) {
            ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
            ASSERT_TRUE(_opCtx->writesAreReplicated());
            ASSERT_FALSE(_replCoord->canAcceptWritesForDatabase(_opCtx.get(), _nss.db()));
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    _createCollection(_opCtx.get(), _nss);

    ASSERT_TRUE(AutoGetDb(_opCtx.get(), _nss.dbName(), MODE_X).getDb());

    auto status = dropDatabaseForApplyOps(_opCtx.get(), _nss.db().toString());
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, status);
    ASSERT_EQUALS(status.reason(),
                  std::string(str::stream() << "Could not drop database " << _nss.db()
                                            << " because we transitioned from PRIMARY to SECONDARY"
                                            << " while waiting for 1 pending collection drop(s)."));

    // Check drop-pending flag in Database after dropDatabase() fails.
    AutoGetDb autoDb(_opCtx.get(), _nss.dbName(), MODE_X);
    auto db = autoDb.getDb();
    ASSERT_TRUE(db);
    ASSERT_FALSE(db->isDropPending(_opCtx.get()));
}

TEST_F(DropDatabaseTest, DropDatabaseFailsToDropAdmin) {
    NamespaceString adminNSS(NamespaceString::kAdminDb, "foo");
    _createCollection(_opCtx.get(), adminNSS);
    ASSERT_THROWS_CODE_AND_WHAT(dropDatabaseForApplyOps(_opCtx.get(), adminNSS.db().toString()),
                                AssertionException,
                                ErrorCodes::IllegalOperation,
                                "Dropping the 'admin' database is prohibited.");
}

}  // namespace
