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
    SizeCountTimestampStoreTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _store = test_helpers::createContainerFastCountStores(operationContext()).timestampStore;
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

    std::unique_ptr<ContainerSizeCountTimestampStore> _store;
};

TEST_F(SizeCountTimestampStoreTest, ReadMassertsWithoutGlobalReadLock) {
    ASSERT_THROWS_CODE(_store->read(operationContext()), DBException, 12915200);
}

TEST_F(SizeCountTimestampStoreTest, WriteMassertsWithoutWriteUnitOfWork) {
    ASSERT_THROWS_CODE(_store->write(operationContext(), Timestamp(10, 1)), DBException, 12280400);
}

TEST_F(SizeCountTimestampStoreTest, WriteMassertsWithoutGlobalWriteLock) {
    auto opCtx = operationContext();
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(_store->write(opCtx, Timestamp(10, 1)), DBException, 12915201);
}

TEST_F(SizeCountTimestampStoreTest, ReadReturnsNoneWhenEmpty) {
    EXPECT_FALSE(readTs().has_value());
}

TEST_F(SizeCountTimestampStoreTest, ReadWriteRoundTripNewEntry) {
    writeTs(Timestamp(10, 1));

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

TEST_F(SizeCountTimestampStoreTest, WriteUpdatesExistingDocument) {
    writeTs(Timestamp(10, 1));
    writeTs(Timestamp(20, 2));

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(20, 2), *result);
}

TEST_F(SizeCountTimestampStoreTest, WriteWithSameTimestampIsIdempotent) {
    writeTs(Timestamp(10, 1));
    writeTs(Timestamp(10, 1));

    const auto result = readTs();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
