/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count {
namespace {
class ReplicatedFastCountInitTest : public CatalogTestFixture {
public:
    ReplicatedFastCountInitTest() : CatalogTestFixture() {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
        _fastCountManager = &ReplicatedFastCountManager::get(_opCtx->getServiceContext());
    }

    void tearDown() override {
        _fastCountManager->shutdown(_opCtx);
        CatalogTestFixture::tearDown();
    }

    OperationContext* _opCtx;
    ReplicatedFastCountManager* _fastCountManager;
};

const NamespaceString replicatedFastCountStoreNss =
    NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);

const NamespaceString replicatedFastCountStoreTimestampsNss =
    NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);

TEST_F(ReplicatedFastCountInitTest,
       setUpReplicatedFastCountCreatesInternalCollectionsAndStartsUpThread) {
    {
        auto coll = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                _opCtx, replicatedFastCountStoreNss, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        ASSERT(!coll.exists());

        auto collTimestamps = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                _opCtx, replicatedFastCountStoreTimestampsNss, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        ASSERT(!collTimestamps.exists());
    }

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    {
        auto coll = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                _opCtx, replicatedFastCountStoreNss, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        ASSERT(coll.exists());

        auto collTimestamps = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                _opCtx, replicatedFastCountStoreTimestampsNss, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        ASSERT(collTimestamps.exists());
    }

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountCreatesRecordStoreIdents) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    // Verify idents do not exist before setup.
    EXPECT_FALSE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    // Verify both idents exist after setup.
    EXPECT_TRUE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountIdempotentIdents) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* engine = storageEngine->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    EXPECT_FALSE(engine->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    EXPECT_FALSE(engine->hasIdent(*ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    setUpReplicatedFastCount(_opCtx);

    EXPECT_TRUE(engine->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    EXPECT_TRUE(engine->hasIdent(*ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);

    // Calling setup a second time should succeed and the idents should still exist.
    _fastCountManager->shutdown(_opCtx);
    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), false);

    // Write a record to the metadata store so we can verify it is preserved on re-setup.
    {
        auto [metadataSCS, _] = _fastCountManager->getSizeCountStores_ForTest();
        auto metadataContainerSCS = dynamic_cast<ContainerSizeCountStore*>(metadataSCS);
        ASSERT(metadataContainerSCS);
        auto metadataRS = metadataContainerSCS->rs_ForTest();

        WriteUnitOfWork wuow(_opCtx);
        std::string key = "test_key";
        RecordId rid(std::span<const char>(key.data(), key.size()));
        const char data[] = "value";
        ASSERT_OK(metadataRS->insertRecord(_opCtx, *ru, rid, data, sizeof(data), Timestamp{}));
        wuow.commit();
        EXPECT_EQ(metadataRS->numRecords(), 1);
    }

    setUpReplicatedFastCount(_opCtx);

    EXPECT_TRUE(engine->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    EXPECT_TRUE(engine->hasIdent(*ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);

    // Verify the previously written record is still present.
    {
        auto [metadataSCS, _] = _fastCountManager->getSizeCountStores_ForTest();
        auto metadataContainerSCS = dynamic_cast<ContainerSizeCountStore*>(metadataSCS);
        ASSERT(metadataContainerSCS);
        auto metadataRS = metadataContainerSCS->rs_ForTest();

        auto cursor = metadataRS->getCursor(_opCtx, *ru);
        EXPECT_TRUE(cursor->next());
    }
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountSkipsContainersWhenFlagDisabled) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", false);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", false);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    setUpReplicatedFastCount(_opCtx);

    // Containers should not be created when the flag is disabled.
    EXPECT_FALSE(
        storageEngine->getEngine()->hasIdent(*ru, std::string(ident::kFastCountMetadataStore)));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *ru, std::string(ident::kFastCountMetadataStoreTimestamps)));

    // Collections and manager should still be set up.
    EXPECT_EQ(_fastCountManager->isRunning_ForTest(), true);
}

TEST_F(ReplicatedFastCountInitTest, StartingUpThenShuttingDownDoesNotHang) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    const int numIterations = 100;
    for (int i = 0; i < numIterations; ++i) {
        setUpReplicatedFastCount(_opCtx);
        _fastCountManager->shutdown(_opCtx);
    }
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountCreatesBothWhenOnlyMetadataExists) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* engine = storageEngine->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Pre-create only the metadata ident to simulate partial state from a previous failure.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStore,
                                            RecordStore::Options{.keyFormat = KeyFormat::String}));
        wuow.commit();
    }

    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    setUpReplicatedFastCount(_opCtx);

    // Both idents should exist after setup creates the timestamps ident.
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountFailsWhenOnlyNonEmptyMetadataExists) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* engine = storageEngine->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Pre-create only the metadata ident and write a record to make it non-empty.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStore,
                                            RecordStore::Options{.keyFormat = KeyFormat::String}));
        wuow.commit();
    }

    {
        auto rs = engine->getRecordStore(_opCtx,
                                         NamespaceString::kAdminCommandNamespace,
                                         ident::kFastCountMetadataStore,
                                         RecordStore::Options{.keyFormat = KeyFormat::String},
                                         boost::none);
        WriteUnitOfWork wuow(_opCtx);
        std::string key = "test_key";
        RecordId rid(std::span<const char>(key.data(), key.size()));
        const char data[] = "value";
        ASSERT_OK(rs->insertRecord(_opCtx, *ru, rid, data, sizeof(data), Timestamp{}));
        wuow.commit();
    }

    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    // Setup should fail because the existing metadata ident is non-empty.
    ASSERT_THROWS(setUpReplicatedFastCount(_opCtx), AssertionException);
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountCreatesBothWhenOnlyTimestampsExists) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* engine = storageEngine->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Pre-create only the timestamps ident to simulate partial state from a previous failure.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStoreTimestamps,
                                            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    setUpReplicatedFastCount(_opCtx);

    // Both idents should exist after setup creates the metadata ident.
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountFailsWhenOnlyNonEmptyTimestampsExists) {
    unittest::ServerParameterGuard ffDurability("featureFlagReplicatedFastCountDurability", true);
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);

    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto* engine = storageEngine->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Pre-create only the timestamps ident and write a record to make it non-empty.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStoreTimestamps,
                                            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    {
        auto rs = engine->getRecordStore(_opCtx,
                                         NamespaceString::kAdminCommandNamespace,
                                         ident::kFastCountMetadataStoreTimestamps,
                                         RecordStore::Options{.keyFormat = KeyFormat::Long},
                                         boost::none);
        WriteUnitOfWork wuow(_opCtx);
        const char data[] = "value";
        ASSERT_OK(rs->insertRecord(_opCtx, *ru, data, sizeof(data), Timestamp{}));
        wuow.commit();
    }

    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    // Setup should fail because the existing timestamps ident is non-empty.
    ASSERT_THROWS(setUpReplicatedFastCount(_opCtx), AssertionException);
}

TEST_F(ReplicatedFastCountInitTest, handleExistingFastCountIdentFailsOnNonEmptyIdent) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Create the metadata ident and write a record so it is non-empty.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStore,
                                            RecordStore::Options{.keyFormat = KeyFormat::String}));
        wuow.commit();
    }
    {
        auto rs = engine->getRecordStore(_opCtx,
                                         NamespaceString::kAdminCommandNamespace,
                                         ident::kFastCountMetadataStore,
                                         RecordStore::Options{.keyFormat = KeyFormat::String},
                                         boost::none);
        WriteUnitOfWork wuow(_opCtx);
        std::string key = "test_key";
        RecordId rid(std::span<const char>(key.data(), key.size()));
        const char data[] = "value";
        ASSERT_OK(rs->insertRecord(_opCtx, *ru, rid, data, sizeof(data), Timestamp{}));
        wuow.commit();
    }

    auto [status, msg] = handleExistingFastCountIdent(_opCtx,
                                                      NamespaceString::kAdminCommandNamespace,
                                                      ident::kFastCountMetadataStore,
                                                      KeyFormat::String);

    // A non-empty existing ident cannot be re-used, so the call returns an error and no message.
    EXPECT_EQ(status.code(), 12309402);
    EXPECT_TRUE(msg.empty());
}

TEST_F(ReplicatedFastCountInitTest, handleExistingFastCountIdentReusesEmptyStringIdent) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Create the metadata ident but leave it empty.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStore,
                                            RecordStore::Options{.keyFormat = KeyFormat::String}));
        wuow.commit();
    }

    auto [status, msg] = handleExistingFastCountIdent(_opCtx,
                                                      NamespaceString::kAdminCommandNamespace,
                                                      ident::kFastCountMetadataStore,
                                                      KeyFormat::String);

    // An empty existing ident can be re-used, so the call succeeds with a descriptive message
    // referencing the ident.
    ASSERT_OK(status);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find(std::string(ident::kFastCountMetadataStore)), std::string::npos);
}

TEST_F(ReplicatedFastCountInitTest, handleExistingFastCountIdentReusesEmptyLongIdent) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Create the timestamps ident (Long key format) but leave it empty.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStoreTimestamps,
                                            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    auto [status, msg] = handleExistingFastCountIdent(_opCtx,
                                                      NamespaceString::kAdminCommandNamespace,
                                                      ident::kFastCountMetadataStoreTimestamps,
                                                      KeyFormat::Long);

    ASSERT_OK(status);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find(std::string(ident::kFastCountMetadataStoreTimestamps)), std::string::npos);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
