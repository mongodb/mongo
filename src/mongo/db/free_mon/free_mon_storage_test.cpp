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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/free_mon/free_mon_storage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class FreeMonStorageTest : public ServiceContextMongoDTest {
private:
    void setUp() final;
    void tearDown() final;

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    ServiceContext::UniqueOperationContext _opCtx;

    executor::NetworkInterfaceMock* _mockNetwork{nullptr};

    std::unique_ptr<executor::ThreadPoolTaskExecutor> _mockThreadPool;

    repl::StorageInterface* _storage{nullptr};
};

void FreeMonStorageTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    _storage = repl::StorageInterface::get(service);

    // Transition to PRIMARY so that the server can accept writes.
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_PRIMARY));

    repl::createOplog(_opCtx.get());
}

void FreeMonStorageTest::tearDown() {
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

repl::ReplicationCoordinatorMock* FreeMonStorageTest::_getReplCoord() const {
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT(replCoord) << "No ReplicationCoordinator installed";
    auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock) << "Unexpected type for installed ReplicationCoordinator";
    return replCoordMock;
}

// Positive: Test Storage works
TEST_F(FreeMonStorageTest, TestStorage) {

    // Validate no collection works
    {
        auto emptyDoc = FreeMonStorage::read(_opCtx.get());
        ASSERT_FALSE(emptyDoc.is_initialized());
    }

    // Create collection with one document.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC = _storage->createCollection(
        _opCtx.get(), NamespaceString("admin", "system.version"), collectionOptions);
    ASSERT_OK(statusCC);


    FreeMonStorageState initialState =
        FreeMonStorageState::parse(IDLParserContext("foo"),
                                   BSON("version" << 1LL << "state"
                                                  << "enabled"
                                                  << "registrationId"
                                                  << "1234"
                                                  << "informationalURL"
                                                  << "http://example.com"
                                                  << "message"
                                                  << "hello"
                                                  << "userReminder"
                                                  << ""));

    {
        auto emptyDoc = FreeMonStorage::read(_opCtx.get());
        ASSERT_FALSE(emptyDoc.is_initialized());
    }

    FreeMonStorage::replace(_opCtx.get(), initialState);

    {
        auto persistedDoc = FreeMonStorage::read(_opCtx.get());

        ASSERT_TRUE(persistedDoc.is_initialized());

        ASSERT_TRUE(persistedDoc == initialState);
    }

    FreeMonStorage::deleteState(_opCtx.get());

    {
        auto emptyDoc = FreeMonStorage::read(_opCtx.get());
        ASSERT_FALSE(emptyDoc.is_initialized());
    }

    // Verfiy delete of nothing succeeds
    FreeMonStorage::deleteState(_opCtx.get());
}


// Positive: Test Storage works on a secondary
TEST_F(FreeMonStorageTest, TestSecondary) {

    // Create collection with one document.
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC = _storage->createCollection(
        _opCtx.get(), NamespaceString("admin", "system.version"), collectionOptions);
    ASSERT_OK(statusCC);


    FreeMonStorageState initialState =
        FreeMonStorageState::parse(IDLParserContext("foo"),
                                   BSON("version" << 1LL << "state"
                                                  << "enabled"
                                                  << "registrationId"
                                                  << "1234"
                                                  << "informationalURL"
                                                  << "http://example.com"
                                                  << "message"
                                                  << "hello"
                                                  << "userReminder"
                                                  << ""));

    FreeMonStorage::replace(_opCtx.get(), initialState);

    {
        auto persistedDoc = FreeMonStorage::read(_opCtx.get());

        ASSERT_TRUE(persistedDoc.is_initialized());

        ASSERT_TRUE(persistedDoc == initialState);
    }

    // Now become a secondary
    ASSERT_OK(_getReplCoord()->setFollowerMode(repl::MemberState::RS_SECONDARY));

    FreeMonStorageState updatedState =
        FreeMonStorageState::parse(IDLParserContext("foo"),
                                   BSON("version" << 2LL << "state"
                                                  << "enabled"
                                                  << "registrationId"
                                                  << "1234"
                                                  << "informationalURL"
                                                  << "http://example.com"
                                                  << "message"
                                                  << "hello"
                                                  << "userReminder"
                                                  << ""));


    {
        auto persistedDoc = FreeMonStorage::read(_opCtx.get());

        ASSERT_TRUE(persistedDoc.is_initialized());

        ASSERT_TRUE(persistedDoc == initialState);
    }

    FreeMonStorage::deleteState(_opCtx.get());

    {
        auto persistedDoc = FreeMonStorage::read(_opCtx.get());
        ASSERT_TRUE(persistedDoc.is_initialized());
    }

    // Verfiy delete of nothing succeeds
    FreeMonStorage::deleteState(_opCtx.get());
}

void insertDoc(OperationContext* optCtx, const NamespaceString nss, StringData id) {
    auto storageInterface = repl::StorageInterface::get(optCtx);

    Lock::DBLock dblk(optCtx, nss.dbName(), MODE_IX);
    Lock::CollectionLock lk(optCtx, nss, MODE_IX);

    BSONObj fakeDoc = BSON("_id" << id);
    BSONElement elementKey = fakeDoc.firstElement();

    ASSERT_OK(storageInterface->upsertById(optCtx, nss, elementKey, fakeDoc));
}

// Positive: Test local.clustermanager
TEST_F(FreeMonStorageTest, TestClusterManagerStorage) {
    const NamespaceString localClusterManagerNss("local.clustermanager");

    // Verify read of non-existent collection works
    ASSERT_FALSE(FreeMonStorage::readClusterManagerState(_opCtx.get()).is_initialized());

    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC =
        _storage->createCollection(_opCtx.get(), localClusterManagerNss, collectionOptions);
    ASSERT_OK(statusCC);

    // Verify read of empty collection works
    ASSERT_FALSE(FreeMonStorage::readClusterManagerState(_opCtx.get()).is_initialized());

    insertDoc(_opCtx.get(), localClusterManagerNss, "foo1");

    // Verify read of singleton collection works
    ASSERT_TRUE(FreeMonStorage::readClusterManagerState(_opCtx.get()).is_initialized());

    insertDoc(_opCtx.get(), localClusterManagerNss, "bar1");

    // Verify read of two doc collection fails
    ASSERT_FALSE(FreeMonStorage::readClusterManagerState(_opCtx.get()).is_initialized());
}
}  // namespace
}  // namespace mongo
