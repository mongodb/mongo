/**
 *    Copyright 2017 MongoDB Inc.
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

#include <set>

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

/**
 * Mock OpObserver that tracks dropped collections and databases.
 */
class OpObserverMock : public OpObserverNoop {
public:
    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) override;
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) override;

    std::set<std::string> droppedDatabaseNames;
    std::set<NamespaceString> droppedCollectionNames;
};

void OpObserverMock::onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    OpObserverNoop::onDropDatabase(opCtx, dbName);
    // Do not update 'droppedDatabaseNames' if OpObserverNoop::onDropDatabase() throws.
    droppedDatabaseNames.insert(dbName);
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid) {
    ASSERT_TRUE(opCtx->lockState()->inAWriteUnitOfWork());
    auto opTime = OpObserverNoop::onDropCollection(opCtx, collectionName, uuid);
    // Do not update 'droppedCollectionNames' if OpObserverNoop::onDropCollection() throws.
    droppedCollectionNames.insert(collectionName);
    return opTime;
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

    repl::StorageInterface::set(service, stdx::make_unique<repl::StorageInterfaceMock>());
    repl::DropPendingCollectionReaper::set(
        service,
        stdx::make_unique<repl::DropPendingCollectionReaper>(repl::StorageInterface::get(service)));

    // Set up ReplicationCoordinator and create oplog.
    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorMock>(service);
    _replCoord = replCoord.get();
    repl::ReplicationCoordinator::set(service, std::move(replCoord));
    repl::setOplogCollectionName();
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    ASSERT_TRUE(_replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for collection and database drops.
    auto opObserver = stdx::make_unique<OpObserverMock>();
    _opObserver = opObserver.get();
    service->setOpObserver(std::move(opObserver));

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
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_TRUE(db->createCollection(opCtx, nss.ns()));
        wuow.commit();
    });

    ASSERT_TRUE(AutoGetCollectionForRead(opCtx, nss).getCollection());
}

TEST_F(DropDatabaseTest, DropDatabaseReturnsNamespaceNotFoundIfDatabaseDoesNotExist) {
    ASSERT_FALSE(AutoGetDb(_opCtx.get(), _nss.db(), MODE_X).getDb());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, dropDatabase(_opCtx.get(), _nss.db().toString()));
}

TEST_F(DropDatabaseTest, DropDatabaseReturnsNotMasterIfNotPrimary) {
    _createCollection(_opCtx.get(), _nss);
    ASSERT_TRUE(_replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(_replCoord->canAcceptWritesForDatabase(_opCtx.get(), _nss.db()));
    ASSERT_EQUALS(ErrorCodes::NotMaster, dropDatabase(_opCtx.get(), _nss.db().toString()));
}

/**
 * Tests successful drop of a database containing a single collection.
 * Checks expected number of onDropCollection() and onDropDatabase() invocations on the
 * OpObserver.
 */
void _testDropDatabase(OperationContext* opCtx,
                       OpObserverMock* opObserver,
                       const NamespaceString& nss,
                       bool expectedOnDropCollection) {
    _createCollection(opCtx, nss);

    ASSERT_TRUE(AutoGetDb(opCtx, nss.db(), MODE_X).getDb());
    ASSERT_OK(dropDatabase(opCtx, nss.db().toString()));
    ASSERT_FALSE(AutoGetDb(opCtx, nss.db(), MODE_X).getDb());

    ASSERT_EQUALS(1U, opObserver->droppedDatabaseNames.size());
    ASSERT_EQUALS(nss.db().toString(), *(opObserver->droppedDatabaseNames.begin()));

    if (expectedOnDropCollection) {
        ASSERT_EQUALS(1U, opObserver->droppedCollectionNames.size());
        ASSERT_EQUALS(nss, *(opObserver->droppedCollectionNames.begin()));
    } else {
        ASSERT_EQUALS(0U, opObserver->droppedCollectionNames.size());
    }
}

TEST_F(DropDatabaseTest, DropDatabaseDoesNotNotifyOpObserverOfDroppedUserCollection) {
    _testDropDatabase(_opCtx.get(), _opObserver, _nss, false);
}

TEST_F(DropDatabaseTest, DropDatabaseDoesNotNotifyOpObserverOfDroppedReplicatedSystemCollection) {
    NamespaceString replicatedSystemNss(_nss.getSisterNS("system.js"));
    _testDropDatabase(_opCtx.get(), _opObserver, replicatedSystemNss, false);
}

TEST_F(DropDatabaseTest, DropDatabaseSkipsDropPendingCollectionWhenDroppingCollections) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dpns = _nss.makeDropPendingNamespace(dropOpTime);
    _testDropDatabase(_opCtx.get(), _opObserver, dpns, false);
}

TEST_F(DropDatabaseTest, DropDatabaseSkipsSystemDotIndexesCollectionWhenDroppingCollections) {
    NamespaceString systemDotIndexesNss(_nss.getSystemIndexesCollection());
    _testDropDatabase(_opCtx.get(), _opObserver, systemDotIndexesNss, false);
}

TEST_F(DropDatabaseTest, DropDatabaseSkipsSystemNamespacesCollectionWhenDroppingCollections) {
    NamespaceString systemNamespacesNss(_nss.getSisterNS("system.namespaces"));
    _testDropDatabase(_opCtx.get(), _opObserver, systemNamespacesNss, false);
}

TEST_F(DropDatabaseTest, DropDatabaseSkipsSystemProfileCollectionWhenDroppingCollections) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    NamespaceString profileNss(_nss.getSisterNS("system.profile"));
    _testDropDatabase(_opCtx.get(), _opObserver, profileNss, false);
}

}  // namespace
