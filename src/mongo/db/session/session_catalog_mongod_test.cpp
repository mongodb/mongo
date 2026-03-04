/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/session/session_catalog_mongod.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/session/sessions_collection_mock.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <utility>

namespace mongo {
namespace {

class TestPersistenceProviderCustomizableSupportImageCollection
    : public mongo::rss::AttachedPersistenceProvider {
public:
    explicit TestPersistenceProviderCustomizableSupportImageCollection(bool supportsImageCollection)
        : mongo::rss::AttachedPersistenceProvider(),
          _supportsFindAndModifyImageCollection(supportsImageCollection) {}

    std::string name() const override {
        return "TestPersistenceProviderCustomizableSupportImageCollection";
    }

    bool supportsFindAndModifyImageCollection() const override {
        return _supportsFindAndModifyImageCollection;
    }

private:
    bool _supportsFindAndModifyImageCollection;
};

class MongoDSessionCatalogTest : public ServiceContextMongoDTest {
protected:
    MongoDSessionCatalogTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        const auto service = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx);
    }

    ClockSourceMock* clock() {
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    ServiceContext::UniqueOperationContext _uniqueOpCtx{makeOperationContext()};
    OperationContext* _opCtx{_uniqueOpCtx.get()};

    std::shared_ptr<MockSessionsCollectionImpl> _collectionMock{
        std::make_shared<MockSessionsCollectionImpl>()};

    std::shared_ptr<SessionsCollection> _collection{
        std::make_shared<MockSessionsCollection>(_collectionMock)};
};

TEST_F(MongoDSessionCatalogTest, ReapSomeExpiredSomeNot) {
    // Create some "old" sessions
    DBDirectClient client(_opCtx);
    SessionTxnRecord txn1(
        makeLogicalSessionIdForTest(), 100, repl::OpTime(Timestamp(100), 1), clock()->now());
    SessionTxnRecord txn2(
        makeLogicalSessionIdForTest(), 200, repl::OpTime(Timestamp(200), 1), clock()->now());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace,
                  std::vector{txn1.toBSON(), txn2.toBSON()});

    // Add some "new" sessions to ensure they don't get reaped
    clock()->advance(Minutes{31});
    _collectionMock->add(LogicalSessionRecord(makeLogicalSessionIdForTest(), clock()->now()));
    _collectionMock->add(LogicalSessionRecord(makeLogicalSessionIdForTest(), clock()->now()));

    auto mongoDSessionCatalog =
        MongoDSessionCatalog(std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>());
    auto numReaped = mongoDSessionCatalog.reapSessionsOlderThan(
        _opCtx, *_collection, clock()->now() - Minutes{30});

    ASSERT_EQ(2, numReaped);
}

TEST_F(MongoDSessionCatalogTest, StepUpCreatesConfigImageCollectionIfSupported) {
    const auto service = getServiceContext();
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

    auto testProvider = std::make_unique<TestPersistenceProviderCustomizableSupportImageCollection>(
        true /* supportsImageCollection */);
    auto& rss = rss::ReplicatedStorageService::get(service);
    rss.setPersistenceProvider(std::move(testProvider));

    // Verify that the image collection does not exist before setup. checkIfNamespaceExists
    // returns OK when the namespace does not exist, and NamespaceExists when it does.
    ASSERT_OK(catalog::checkIfNamespaceExists(_opCtx, NamespaceString::kConfigImagesNamespace));

    MongoDSessionCatalog::set(
        service,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx);
    mongoDSessionCatalog->onStepUp(_opCtx);

    // Verify that the image collection exists after stepup.
    ASSERT_EQ(catalog::checkIfNamespaceExists(_opCtx, NamespaceString::kConfigImagesNamespace),
              ErrorCodes::NamespaceExists);

    // To test idempotency, step up again.
    mongoDSessionCatalog->onStepUp(_opCtx);

    // Verify that the image collection still exists.
    ASSERT_EQ(catalog::checkIfNamespaceExists(_opCtx, NamespaceString::kConfigImagesNamespace),
              ErrorCodes::NamespaceExists);
}

TEST_F(MongoDSessionCatalogTest, StepUpDoesNotCreateConfigImageCollectionIfNotSupported) {
    const auto service = getServiceContext();
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

    auto testProvider = std::make_unique<TestPersistenceProviderCustomizableSupportImageCollection>(
        false /* supportsImageCollection */);
    auto& rss = rss::ReplicatedStorageService::get(service);
    rss.setPersistenceProvider(std::move(testProvider));

    // Verify that the image collection does not exist before setup. checkIfNamespaceExists
    // returns OK when the namespace does not exist, and NamespaceExists when it does.
    ASSERT_OK(catalog::checkIfNamespaceExists(_opCtx, NamespaceString::kConfigImagesNamespace));

    MongoDSessionCatalog::set(
        service,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx);
    mongoDSessionCatalog->onStepUp(_opCtx);

    // Verify that the image collection does not exist after stepup.
    ASSERT_OK(catalog::checkIfNamespaceExists(_opCtx, NamespaceString::kConfigImagesNamespace));
}

}  // namespace
}  // namespace mongo
