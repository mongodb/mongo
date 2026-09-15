// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/record_store_write_conflict_fail_points.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo::replicated_fast_count::flusher {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class FlusherTest : public CatalogTestFixture {
public:
    FlusherTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        auto stores = test_helpers::createContainerFastCountStores(operationContext());
        sizeCountStore = std::move(stores.sizeCountStore);
        timestampStore = std::move(stores.timestampStore);
    }

    boost::optional<Timestamp> readTimestampStore() {
        Lock::GlobalLock lk(operationContext(), MODE_IS);
        return timestampStore->read(operationContext());
    }

    boost::optional<SizeCountStore::Entry> readSizeCount(UUID uuid) {
        Lock::GlobalLock lk(operationContext(), MODE_IS);
        return sizeCountStore->read(operationContext(), uuid);
    }

    std::unique_ptr<SizeCountStore> sizeCountStore;
    std::unique_ptr<SizeCountTimestampStore> timestampStore;

    const test_helpers::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest("flusher_test", "collA"),
        .uuid = UUID::gen(),
    };
};

TEST_F(FlusherTest, FlushPersistsBatch) {
    const OplogScanResult batch{
        .deltas = {{collA.uuid,
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}}},
        .lastTimestamp = Timestamp(1, 2)};

    const auto result = flush(operationContext(), *sizeCountStore, *timestampStore, batch);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->previousValidAsOfTS, Timestamp::min());
    EXPECT_EQ(result->newValidAsOfTS, Timestamp(1, 2));
    EXPECT_EQ(result->checkpointBufferSize, 1);
    EXPECT_EQ(result->entryWriteCount, 1);
    EXPECT_EQ(result->flushAttempts, 1);

    const auto entry = readSizeCount(collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->timestamp, Timestamp(1, 2));
    EXPECT_EQ(entry->size, 30);
    EXPECT_EQ(entry->count, 2);

    EXPECT_EQ(readTimestampStore(), boost::optional<Timestamp>(Timestamp(1, 2)));
}

TEST_F(FlusherTest, FlushWithNoNewWorkIsNoOp) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "Skipping test due to OTel metrics being unavailable in this build";
    }

    // No persisted timestamp.
    {
        const OplogScanResult batch{
            .deltas = {},
            .lastTimestamp = Timestamp::min(),
        };
        EXPECT_FALSE(
            flush(operationContext(), *sizeCountStore, *timestampStore, batch).has_value());
    }

    // Persisted timestamp (1, 1).
    {
        const Timestamp persistedTs(1, 1);
        test_helpers::insertSizeCountTimestamp(operationContext(), *timestampStore, persistedTs);

        const OplogScanResult batch{
            .deltas = {},
            .lastTimestamp = persistedTs,
        };
        EXPECT_FALSE(
            flush(operationContext(), *sizeCountStore, *timestampStore, batch).has_value());
    }
}

TEST_F(FlusherTest, FlushRetriesOnWriteConflict) {
    auto failPoint = enableWriteConflictForWrites(
        FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});

    const OplogScanResult batch{
        .deltas = {{collA.uuid,
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}},
        .lastTimestamp = Timestamp(1, 1)};
    const auto result = flush(operationContext(), *sizeCountStore, *timestampStore, batch);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->flushAttempts, 2);

    const auto entry = readSizeCount(collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->timestamp, Timestamp(1, 1));
    EXPECT_EQ(entry->size, 10);
    EXPECT_EQ(entry->count, 1);
}

using FlusherTestDeathTest = FlusherTest;

DEATH_TEST_REGEX_F(FlusherTestDeathTest, ValidAsOfDidNotAdvanceTasserts, "12101801") {
    const Timestamp persistedTs(1, 1);
    test_helpers::insertSizeCountTimestamp(operationContext(), *timestampStore, persistedTs);

    const OplogScanResult batch{
        .deltas = {{collA.uuid,
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}},
        .lastTimestamp = persistedTs};
    flush(operationContext(), *sizeCountStore, *timestampStore, batch);
}

}  // namespace
}  // namespace mongo::replicated_fast_count::flusher
