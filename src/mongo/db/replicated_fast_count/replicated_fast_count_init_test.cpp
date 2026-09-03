// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
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

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountCreatesRecordStoreIdents) {
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

TEST_F(ReplicatedFastCountInitTest, StartingUpThenShuttingDownDoesNotHang) {
    const int numIterations = 100;
    for (int i = 0; i < numIterations; ++i) {
        setUpReplicatedFastCount(_opCtx);
        _fastCountManager->shutdown(_opCtx);
    }
}

TEST_F(ReplicatedFastCountInitTest, setUpReplicatedFastCountCreatesBothWhenOnlyMetadataExists) {
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

TEST_F(ReplicatedFastCountInitTest, dropInternalFastCountContainersRemovesExistingIdents) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Create both container idents directly, as they would exist on disk when a previously
    // running node begins a resync. This deliberately does not bind the manager to them, matching
    // the state during initial sync's drop phase.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStore,
                                            RecordStore::Options{.keyFormat = KeyFormat::String}));
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStoreTimestamps,
                                            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    // Write a stale record into the metadata container to represent leftover per-collection state.
    {
        auto rs = engine->getRecordStore(_opCtx,
                                         NamespaceString::kAdminCommandNamespace,
                                         ident::kFastCountMetadataStore,
                                         RecordStore::Options{.keyFormat = KeyFormat::String},
                                         boost::none);
        WriteUnitOfWork wuow(_opCtx);
        std::string key = "stale_key";
        RecordId rid(std::span<const char>(key.data(), key.size()));
        const char data[] = "value";
        ASSERT_OK(rs->insertRecord(_opCtx, *ru, rid, data, sizeof(data), Timestamp{}));
        wuow.commit();
    }

    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    dropInternalFastCountContainers(_opCtx);

    // The drop is scheduled as an immediate drop-pending ident. Complete the pending drops to
    // observe that the tables -- and therefore the stale record -- are gone.
    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    ASSERT_OK(
        storageEngine->immediatelyCompletePendingDrop(_opCtx, ident::kFastCountMetadataStore));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(
        _opCtx, ident::kFastCountMetadataStoreTimestamps));

    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(ReplicatedFastCountInitTest, dropInternalFastCountContainersIsNoOpWhenAbsent) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);

    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    // Dropping when the containers do not exist must not throw or create anything.
    dropInternalFastCountContainers(_opCtx);

    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(ReplicatedFastCountInitTest, dropInternalFastCountContainersHandlesPartialState) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Only the metadata ident exists (e.g. a partial prior state). The drop must remove it and
    // leave the (absent) timestamps ident handling as a no-op.
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

    dropInternalFastCountContainers(_opCtx);

    // Complete the scheduled drop-pending drop; the absent timestamps ident is a no-op.
    auto* storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    ASSERT_OK(
        storageEngine->immediatelyCompletePendingDrop(_opCtx, ident::kFastCountMetadataStore));

    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_FALSE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));
}

TEST_F(ReplicatedFastCountInitTest, dropInternalFastCountContainersAllowsCleanRecreate) {
    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx);
    auto& provider = rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider();

    // Simulate a stale metadata container carrying a leftover record.
    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(engine->createRecordStore(provider,
                                            *ru,
                                            NamespaceString::kAdminCommandNamespace,
                                            ident::kFastCountMetadataStore,
                                            RecordStore::Options{.keyFormat = KeyFormat::String}));
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
                                         ident::kFastCountMetadataStore,
                                         RecordStore::Options{.keyFormat = KeyFormat::String},
                                         boost::none);
        WriteUnitOfWork wuow(_opCtx);
        std::string key = "stale_key";
        RecordId rid(std::span<const char>(key.data(), key.size()));
        const char data[] = "value";
        ASSERT_OK(rs->insertRecord(_opCtx, *ru, rid, data, sizeof(data), Timestamp{}));
        wuow.commit();
    }

    dropInternalFastCountContainers(_opCtx);

    // After the drop, re-creating the containers succeeds and yields an empty metadata store, i.e.
    // a clean slate with no stale record carried over.
    ASSERT_OK(createInternalFastCountContainers(_opCtx,
                                                NamespaceString::kAdminCommandNamespace,
                                                ident::kFastCountMetadataStore,
                                                KeyFormat::String,
                                                ident::kFastCountMetadataStoreTimestamps,
                                                KeyFormat::Long,
                                                /*writeToOplog=*/false));

    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStore));
    EXPECT_TRUE(engine->hasIdent(*ru, ident::kFastCountMetadataStoreTimestamps));

    auto rs = engine->getRecordStore(_opCtx,
                                     NamespaceString::kAdminCommandNamespace,
                                     ident::kFastCountMetadataStore,
                                     RecordStore::Options{.keyFormat = KeyFormat::String},
                                     boost::none);
    auto cursor = rs->getCursor(_opCtx, *ru);
    EXPECT_FALSE(cursor->next());
}

}  // namespace
}  // namespace mongo::replicated_fast_count
