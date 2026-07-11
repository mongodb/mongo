// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo::replicated_fast_count {
namespace {

enum class Mode { kCollection, kContainer };

// Runs each test case in both collection-backed and container-backed modes.
class SizeCountTimestampStoreTest : public CatalogTestFixture,
                                    public ::testing::WithParamInterface<Mode> {
protected:
    int expectedReadLockCode() const {
        return GetParam() == Mode::kCollection ? 12915206 : 12915200;
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        auto opCtx = operationContext();
        if (GetParam() == Mode::kCollection) {
            ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), opCtx));
            _store = std::make_unique<CollectionSizeCountTimestampStore>();
            return;
        }

        _ffContainerWrites =
            std::make_unique<unittest::ServerParameterGuard>("featureFlagContainerWrites", true);

        ASSERT_OK(createInternalFastCountContainers(opCtx,
                                                    NamespaceString::kAdminCommandNamespace,
                                                    ident::kFastCountMetadataStore,
                                                    KeyFormat::String,
                                                    ident::kFastCountMetadataStoreTimestamps,
                                                    KeyFormat::Long,
                                                    /*writeToOplog=*/false));

        auto* engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
        auto recordStore =
            engine->getRecordStore(opCtx,
                                   NamespaceString::kAdminCommandNamespace,
                                   ident::kFastCountMetadataStoreTimestamps,
                                   RecordStore::Options{.keyFormat = KeyFormat::Long},
                                   /*uuid=*/boost::none);
        _store = std::make_unique<ContainerSizeCountTimestampStore>(std::move(recordStore));
    }

    void writeTs(Timestamp timestamp) {
        Lock::GlobalLock writeLock(operationContext(), MODE_IX);
        WriteUnitOfWork wuow(operationContext());
        _store->write(operationContext(), timestamp);
        wuow.commit();
    }

    boost::optional<Timestamp> readTs() {
        Lock::GlobalLock readLock(operationContext(), MODE_IS);
        return _store->read(operationContext());
    }

    std::unique_ptr<unittest::ServerParameterGuard> _ffContainerWrites;
    std::unique_ptr<SizeCountTimestampStore> _store;
};

TEST_P(SizeCountTimestampStoreTest, ReadMassertsWithoutGlobalReadLock) {
    ASSERT_THROWS_CODE(_store->read(operationContext()), DBException, expectedReadLockCode());
}

TEST_P(SizeCountTimestampStoreTest, WriteMassertsWithoutWriteUnitOfWork) {
    ASSERT_THROWS_CODE(_store->write(operationContext(), Timestamp(10, 1)), DBException, 12280400);
}

TEST_P(SizeCountTimestampStoreTest, WriteMassertsWithoutGlobalWriteLock) {
    auto opCtx = operationContext();
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(_store->write(opCtx, Timestamp(10, 1)), DBException, 12915201);
}

TEST_P(SizeCountTimestampStoreTest, ReadReturnsNoneWhenEmpty) {
    EXPECT_FALSE(readTs().has_value());
}

TEST_P(SizeCountTimestampStoreTest, ReadWriteRoundTripNewEntry) {
    writeTs(Timestamp(10, 1));

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

TEST_P(SizeCountTimestampStoreTest, WriteUpdatesExistingDocument) {
    writeTs(Timestamp(10, 1));
    writeTs(Timestamp(20, 2));

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(20, 2), *result);
}

TEST_P(SizeCountTimestampStoreTest, WriteWithSameTimestampIsIdempotent) {
    writeTs(Timestamp(10, 1));
    writeTs(Timestamp(10, 1));

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

INSTANTIATE_TEST_SUITE_P(,
                         SizeCountTimestampStoreTest,
                         ::testing::Values(Mode::kCollection, Mode::kContainer),
                         [](const ::testing::TestParamInfo<Mode>& info) {
                             return info.param == Mode::kCollection ? "Collection" : "Container";
                         });

// Collection-only case: container mode provisions must pass an existing RecordStore to the
// SizeCountTimestampStore constructor, so there is no equivalent "backing storage does not exist"
// scenario to exercise.
class SizeCountTimestampStoreCollectionModeTest : public CatalogTestFixture {};

TEST_F(SizeCountTimestampStoreCollectionModeTest, ReadReturnsNoneWhenCollectionDoesNotExist) {
    const CollectionSizeCountTimestampStore store;
    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    EXPECT_FALSE(store.read(operationContext()).has_value());
}

}  // namespace
}  // namespace mongo::replicated_fast_count
