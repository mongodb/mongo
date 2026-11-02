// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

namespace mongo::replicated_fast_count {
namespace {

class SizeCountTimestampStoreTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>());
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _store = test_helpers::createContainerFastCountStores(operationContext()).timestampStore;
    }

    boost::optional<Timestamp> insertTs(Timestamp timestamp) {
        return test_helpers::insertSizeCountTimestamp(operationContext(), *_store, timestamp);
    }

    boost::optional<Timestamp> readTs() {
        Lock::GlobalLock readLock(operationContext(), MODE_IS);
        return _store->read(operationContext());
    }

    std::unique_ptr<SizeCountTimestampStore> _store;
};

TEST_F(SizeCountTimestampStoreTest, ReadMassertsWithoutGlobalReadLock) {
    ASSERT_THROWS_CODE(_store->read(operationContext()), DBException, 12915200);
}

TEST_F(SizeCountTimestampStoreTest, InsertMassertsWithoutWriteUnitOfWork) {
    ASSERT_THROWS_CODE(_store->insert(operationContext(), Timestamp(10, 1)), DBException, 12280400);
}

TEST_F(SizeCountTimestampStoreTest, InsertMassertsWithoutGlobalWriteLock) {
    auto opCtx = operationContext();
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(_store->insert(opCtx, Timestamp(10, 1)), DBException, 12915201);
}

TEST_F(SizeCountTimestampStoreTest, ReadReturnsNoneWhenEmpty) {
    EXPECT_FALSE(readTs().has_value());
}

TEST_F(SizeCountTimestampStoreTest, InsertReturnsNoneForNewEntry) {
    EXPECT_FALSE(insertTs(Timestamp(10, 1)).has_value());

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

TEST_F(SizeCountTimestampStoreTest, InsertReturnsOldTimestampAndUpdatesExistingDocument) {
    EXPECT_FALSE(insertTs(Timestamp(10, 1)).has_value());
    const auto oldTimestamp = insertTs(Timestamp(20, 2));
    ASSERT_TRUE(oldTimestamp.has_value());
    EXPECT_EQ(Timestamp(10, 1), *oldTimestamp);

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(20, 2), *result);
}

TEST_F(SizeCountTimestampStoreTest, InsertWithSameTimestampReturnsOldTimestamp) {
    EXPECT_FALSE(insertTs(Timestamp(10, 1)).has_value());
    const auto oldTimestamp = insertTs(Timestamp(10, 1));
    ASSERT_TRUE(oldTimestamp.has_value());
    EXPECT_EQ(Timestamp(10, 1), *oldTimestamp);

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

TEST_F(SizeCountTimestampStoreTest, AbortedInsertLeavesStoreEmpty) {
    {
        auto opCtx = operationContext();
        Lock::GlobalLock writeLock(opCtx, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        EXPECT_FALSE(_store->insert(opCtx, Timestamp(10, 1)).has_value());
    }
    EXPECT_FALSE(readTs().has_value());
    EXPECT_FALSE(insertTs(Timestamp(20, 2)).has_value());
}

TEST_F(SizeCountTimestampStoreTest, AbortedUpdatePreservesOldTimestamp) {
    EXPECT_FALSE(insertTs(Timestamp(10, 1)).has_value());
    {
        auto opCtx = operationContext();
        Lock::GlobalLock writeLock(opCtx, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        const auto oldTimestamp = _store->insert(opCtx, Timestamp(20, 2));
        ASSERT_TRUE(oldTimestamp.has_value());
        EXPECT_EQ(Timestamp(10, 1), *oldTimestamp);
    }
    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
    const auto oldTimestamp = insertTs(Timestamp(30, 3));
    ASSERT_TRUE(oldTimestamp.has_value());
    EXPECT_EQ(Timestamp(10, 1), *oldTimestamp);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
