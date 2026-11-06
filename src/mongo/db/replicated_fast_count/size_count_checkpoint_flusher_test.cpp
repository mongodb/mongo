// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
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
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <array>
#include <utility>

namespace mongo::replicated_fast_count::flusher {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class FlusherTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>());
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        // Register an OpObserver so that watermark entries written in writeWatermark() are actually
        // written to the oplog.
        test_helpers::registerOpObserverForTest(getServiceContext());

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

    boost::optional<FlushResult> flushBatch(const OplogScanResult& batch) {
        const auto previousValidAsOf = readTimestampStore().value_or(Timestamp::min());
        auto result = flush(operationContext(), *sizeCountStore, *timestampStore, batch);
        if (result) {
            EXPECT_EQ(result->previousValidAsOfTS, previousValidAsOf);
            EXPECT_EQ(result->checkpointBufferSize, batch.deltas.size());
            EXPECT_EQ(boost::optional<Timestamp>(result->newValidAsOfTS), batch.lastTimestamp);
            EXPECT_EQ(result->flushAttempts, 1);
            EXPECT_EQ(readTimestampStore(), batch.lastTimestamp);
        }
        return result;
    }

    std::unique_ptr<SizeCountStore> sizeCountStore;
    std::unique_ptr<SizeCountTimestampStore> timestampStore;

    const test_helpers::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest("flusher_test", "collA"),
        .uuid = UUID::gen(),
    };
    const test_helpers::NsAndUUID collB{
        .nss = NamespaceString::createNamespaceString_forTest("flusher_test", "collB"),
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
    EXPECT_EQ(result->writeCounts,
              (CheckpointWriteCounts{.entryWriteCount = 1, .insertCount = 1, .updateCount = 0}));
    EXPECT_EQ(result->flushAttempts, 1);

    const auto entry = readSizeCount(collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->timestamp, Timestamp(1, 2));
    EXPECT_EQ(entry->size, 30);
    EXPECT_EQ(entry->count, 2);

    EXPECT_EQ(readTimestampStore(), boost::optional<Timestamp>(Timestamp(1, 2)));
}

TEST_F(FlusherTest, SplitFlushReplicatesAtomically) {
    // Force the metadata and timestamp writes into separate applyOps entries.
    unittest::ServerParameterGuard maxOpsPerOplogEntry{
        "maxNumberOfBatchedOperationsInSingleOplogEntry", 1};

    const OplogScanResult batch{
        .deltas = {{collA.uuid,
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}}},
        .lastTimestamp = Timestamp(1, 2)};
    ASSERT_TRUE(flush(operationContext(), *sizeCountStore, *timestampStore, batch).has_value());

    const auto entries = test_helpers::getOplogEntriesMatching(
        operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getCommandType() == repl::CommandTypeEnum::kApplyOps;
        });
    ASSERT_EQ(entries.size(), 2);
    EXPECT_TRUE(entries.front().isPartialTransaction());
    EXPECT_FALSE(entries.back().isPartialTransaction());
    EXPECT_EQ(entries.back().getPrevWriteOpTimeInTransaction(), entries.front().getOpTime());
    EXPECT_EQ(entries.back().getObject()["count"].safeNumberLong(), 2);
}

TEST_F(FlusherTest, FlushWithNoNewWorkIsNoOp) {
    // No persisted timestamp.
    {
        const OplogScanResult batch{
            .deltas = {},
            .lastTimestamp = Timestamp::min(),
        };
        EXPECT_FALSE(
            flush(operationContext(), *sizeCountStore, *timestampStore, batch).has_value());
        EXPECT_FALSE(readTimestampStore().has_value());
        EXPECT_FALSE(readSizeCount(collA.uuid).has_value());
        EXPECT_FALSE(readSizeCount(collB.uuid).has_value());
        EXPECT_FALSE(flushBatch(OplogScanResult{}).has_value());
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
        EXPECT_EQ(readTimestampStore(), persistedTs);
        EXPECT_FALSE(readSizeCount(collA.uuid).has_value());
        EXPECT_FALSE(readSizeCount(collB.uuid).has_value());
        EXPECT_FALSE(flushBatch(OplogScanResult{}).has_value());
        EXPECT_EQ(readTimestampStore(), persistedTs);
    }
}

TEST_F(FlusherTest, FlushAdvancesExistingSizeCountAndTimestamp) {
    const Timestamp previousTs(1, 1);
    const Timestamp newTs(1, 3);
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = previousTs, .size = 100, .count = 3});
    test_helpers::insertSizeCountTimestamp(operationContext(), *timestampStore, previousTs);

    const OplogScanResult batch{
        .deltas = {{collA.uuid,
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 60, .count = 2}}}}},
        .lastTimestamp = newTs};
    const auto result = flushBatch(batch);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->writeCounts,
              (CheckpointWriteCounts{.entryWriteCount = 1, .insertCount = 0, .updateCount = 1}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = newTs, .size = 160, .count = 5}));
}

TEST_F(FlusherTest, FlushAdvancesTimestampWithoutMetadataWrites) {
    const SizeCountStore::Entry initial{.timestamp = Timestamp(1, 1), .size = 100, .count = 3};
    test_helpers::insertSizeCountEntry(operationContext(), *sizeCountStore, collA.uuid, initial);
    test_helpers::insertSizeCountTimestamp(operationContext(), *timestampStore, initial.timestamp);

    const auto result = flushBatch({.deltas = {}, .lastTimestamp = Timestamp(50, 1)});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->writeCounts, (CheckpointWriteCounts{}));
    EXPECT_EQ(readSizeCount(collA.uuid), initial);
}

TEST_F(FlusherTest, FlushPersistsZeroNetDelta) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 100, .count = 3});
    const auto result = flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {0, 0}}}}},
         .lastTimestamp = Timestamp(1, 3)});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->writeCounts,
              (CheckpointWriteCounts{.entryWriteCount = 1, .insertCount = 0, .updateCount = 1}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 3), .size = 100, .count = 3}));
}

TEST_F(FlusherTest, FlushTracksCollectionsIndependently) {
    const auto first = flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {50, 1}}}},
                    {collB.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {40, 1}}}}},
         .lastTimestamp = Timestamp(1, 3)});
    ASSERT_TRUE(first);
    EXPECT_EQ(first->writeCounts,
              (CheckpointWriteCounts{.entryWriteCount = 2, .insertCount = 2, .updateCount = 0}));
    const SizeCountStore::Entry expectedA{.timestamp = Timestamp(1, 3), .size = 50, .count = 1};
    EXPECT_EQ(readSizeCount(collA.uuid), expectedA);
    EXPECT_EQ(readSizeCount(collB.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 3), .size = 40, .count = 1}));

    const auto second = flushBatch(
        {.deltas = {{collB.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {-80, -2}}}}},
         .lastTimestamp = Timestamp(1, 4)});
    ASSERT_TRUE(second);
    EXPECT_EQ(second->writeCounts,
              (CheckpointWriteCounts{.entryWriteCount = 1, .insertCount = 0, .updateCount = 1}));
    EXPECT_EQ(readSizeCount(collA.uuid), expectedA);
    EXPECT_EQ(readSizeCount(collB.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 4), .size = -40, .count = -1}));
}

TEST_F(FlusherTest, FlushCreatesCollectionWithAccumulatedInserts) {
    for (const auto sizeCount : std::array{CollectionSizeCount{0, 0}, CollectionSizeCount{30, 2}}) {
        const auto uuid = UUID::gen();
        const Timestamp ts(1, static_cast<unsigned int>(sizeCount.count + 1));
        const auto result =
            flushBatch({.deltas = {{uuid,
                                    ReplicatedMetadataDelta{.metadata = {.sizeCount = sizeCount},
                                                            .state = DDLState::kCreated}}},
                        .lastTimestamp = ts});
        ASSERT_TRUE(result);
        EXPECT_EQ(
            result->writeCounts,
            (CheckpointWriteCounts{.entryWriteCount = 1, .insertCount = 1, .updateCount = 0}));
        EXPECT_EQ(readSizeCount(uuid),
                  (SizeCountStore::Entry{
                      .timestamp = ts, .size = sizeCount.size, .count = sizeCount.count}));
    }
}

TEST_F(FlusherTest, FlushDropsTrackedAndUntrackedCollections) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});
    // Removing an untracked UUID is a no-op, but the global timestamp still advances.
    int increment = 2;
    for (const auto uuid : {collA.uuid, collB.uuid}) {
        const auto result =
            flushBatch({.deltas = {{uuid, ReplicatedMetadataDelta{.state = DDLState::kDropped}}},
                        .lastTimestamp = Timestamp(1, increment++)});
        ASSERT_TRUE(result);
        EXPECT_EQ(result->writeCounts,
                  (CheckpointWriteCounts{.entryWriteCount = uuid == collA.uuid ? 1u : 0u,
                                         .insertCount = 0,
                                         .updateCount = 0}));
        EXPECT_FALSE(readSizeCount(uuid));
    }
}

TEST_F(FlusherTest, FlushDropAndRecreateDiscardsPersistedTotals) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 100, .count = 50});
    const auto result =
        flushBatch({.deltas = {{collA.uuid,
                                ReplicatedMetadataDelta{.metadata = {.sizeCount = {200, 1}},
                                                        .state = DDLState::kDroppedAndRecreated}}},
                    .lastTimestamp = Timestamp(1, 4)});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->writeCounts,
              (CheckpointWriteCounts{.entryWriteCount = 1, .insertCount = 0, .updateCount = 1}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 4), .size = 200, .count = 1}));
}

TEST_F(FlusherTest, FlushPersistsNegativeDeltaForUntrackedCollection) {
    ASSERT_TRUE(flushBatch(
        {.deltas = {{collB.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {-80, -2}}}}},
         .lastTimestamp = Timestamp(1, 2)}));
    EXPECT_EQ(readSizeCount(collB.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 2), .size = -80, .count = -2}));
}

TEST_F(FlusherTest, FlushAppliesRepairHashDiffToPersistedMetadata) {
    const int64_t persistedHash = 0x0f0f0f0f0f0f0f0f;
    const int64_t repairHash = 0x0123456789abcdef;
    test_helpers::insertSizeCountEntry(
        operationContext(),
        *sizeCountStore,
        collA.uuid,
        {.timestamp = Timestamp(0, 1), .size = 1000, .count = 10, .hash = persistedHash});
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collB.uuid,
                                       {.timestamp = Timestamp(0, 1), .size = 1000, .count = 10});
    ASSERT_TRUE(flushBatch(
        {.deltas =
             {{collA.uuid,
               ReplicatedMetadataDelta{.metadata = {.sizeCount = {100, 5}, .hash = repairHash}}},
              {collB.uuid,
               ReplicatedMetadataDelta{.metadata = {.sizeCount = {100, 5}, .hash = repairHash}}}},
         .lastTimestamp = Timestamp(1, 1)}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 1),
                                     .size = 1100,
                                     .count = 15,
                                     .hash = persistedHash ^ repairHash}));
    EXPECT_EQ(readSizeCount(collB.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 1100, .count = 15}));
}

TEST_F(FlusherTest, FlushPersistsPresentAndAbsentHashesForEachDDLState) {
    int increment = 1;
    for (const auto state : {DDLState::kCreated, DDLState::kNone, DDLState::kDroppedAndRecreated}) {
        for (const auto hash :
             std::array{boost::optional<int64_t>{}, boost::optional<int64_t>{0x1234}}) {
            const auto uuid = UUID::gen();
            if (state != DDLState::kCreated) {
                test_helpers::insertSizeCountEntry(
                    operationContext(),
                    *sizeCountStore,
                    uuid,
                    {.timestamp = Timestamp::min(),
                     .size = 10,
                     .count = 2,
                     .hash = hash ? boost::optional<int64_t>{0} : boost::none});
            }
            const Timestamp ts(1, increment++);
            ASSERT_TRUE(
                flushBatch({.deltas = {{uuid,
                                        ReplicatedMetadataDelta{
                                            .metadata = {.sizeCount = {25, 1}, .hash = hash},
                                            .state = state}}},
                            .lastTimestamp = ts}));
            EXPECT_EQ(readSizeCount(uuid),
                      (SizeCountStore::Entry{.timestamp = ts,
                                             .size = state == DDLState::kNone ? 35 : 25,
                                             .count = state == DDLState::kNone ? 3 : 1,
                                             .hash = hash}));
        }
    }
}

TEST_F(FlusherTest, FlushDropAndRecreateTracksHashAcrossBatches) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 1, .count = 2});
    ASSERT_TRUE(
        flushBatch({.deltas = {{collA.uuid, ReplicatedMetadataDelta{.state = DDLState::kDropped}}},
                    .lastTimestamp = Timestamp(1, 1)}));
    EXPECT_FALSE(readSizeCount(collA.uuid));
    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid,
                     ReplicatedMetadataDelta{.metadata = {.sizeCount = {25, 1}, .hash = 0x1234},
                                             .state = DDLState::kCreated}}},
         .lastTimestamp = Timestamp(1, 2)}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{
                  .timestamp = Timestamp(1, 2), .size = 25, .count = 1, .hash = 0x1234}));
}

TEST_F(FlusherTest, FlushDropAndRecreatePreservesAbsentHashWithinBatch) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 1, .count = 2});
    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid,
                     ReplicatedMetadataDelta{.metadata = {.sizeCount = {25, 1}, .hash = 0x1234},
                                             .state = DDLState::kDroppedAndRecreated}}},
         .lastTimestamp = Timestamp(1, 1)}));
    // TODO SERVER-133557: Decide if this is the behavior we want.
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 25, .count = 1}));
}

TEST_F(FlusherTest, FlushAccumulatesOplogMetadataIndependentlyAcrossBatches) {
    // The flusher treats oplog metadata like any other UUID's already-aggregated delta.
    const UUID oplog = UUID::gen();
    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {50, 1}}}},
                    {oplog, ReplicatedMetadataDelta{.metadata = {.sizeCount = {200, 1}}}}},
         .lastTimestamp = Timestamp(1, 1)}));
    EXPECT_EQ(readSizeCount(oplog),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 1}));

    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {50, 0}}}},
                    {oplog, ReplicatedMetadataDelta{.metadata = {.sizeCount = {250, 1}}}}},
         .lastTimestamp = Timestamp(1, 2)}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 2), .size = 100, .count = 1}));
    EXPECT_EQ(readSizeCount(oplog),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 2), .size = 450, .count = 2}));

    // A negative size delta reduces the persisted oplog size without changing its count.
    ASSERT_TRUE(flushBatch(
        {.deltas = {{oplog, ReplicatedMetadataDelta{.metadata = {.sizeCount = {-100, 0}}}}},
         .lastTimestamp = Timestamp(1, 3)}));
    EXPECT_EQ(readSizeCount(oplog),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 3), .size = 350, .count = 2}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 2), .size = 100, .count = 1}));

    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {20, 1}}}},
                    {oplog, ReplicatedMetadataDelta{.metadata = {.sizeCount = {150, 1}}}}},
         .lastTimestamp = Timestamp(1, 5)}));
    EXPECT_EQ(readSizeCount(oplog),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 5), .size = 500, .count = 3}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 5), .size = 120, .count = 2}));
}

TEST_F(FlusherTest, FlushAppliesNegativeAndPositiveDeltasAcrossBatches) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       *sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 200, .count = 5});
    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {-120, -3}}}}},
         .lastTimestamp = Timestamp(1, 2)}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 2), .size = 80, .count = 2}));
    ASSERT_TRUE(flushBatch(
        {.deltas = {{collA.uuid, ReplicatedMetadataDelta{.metadata = {.sizeCount = {70, 1}}}}},
         .lastTimestamp = Timestamp(1, 5)}));
    EXPECT_EQ(readSizeCount(collA.uuid),
              (SizeCountStore::Entry{.timestamp = Timestamp(1, 5), .size = 150, .count = 3}));
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

TEST_F(FlusherTest, WriteWatermarkWritesNoopEntry) {
    OtelMetricsCapturer capturer;

    writeWatermark(operationContext());

    const std::vector<repl::OplogEntry> noopEntries = test_helpers::getOplogEntriesMatching(
        operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getOpType() == repl::OpTypeEnum::kNoop &&
                entry.getObject()["msg"].valueStringData() == kWatermarkMsg;
        });
    ASSERT_EQ(noopEntries.size(), 1);

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountWatermarksWritten), 1);
    }
}

TEST_F(FlusherTest, WriteWatermarkRetriesOnWriteConflict) {
    OtelMetricsCapturer capturer;

    const std::unique_ptr<FailPointEnableBlock> failPoint = enableWriteConflictForWrites(
        FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});

    writeWatermark(operationContext());

    const std::vector<repl::OplogEntry> noopEntries = test_helpers::getOplogEntriesMatching(
        operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getOpType() == repl::OpTypeEnum::kNoop &&
                entry.getObject()["msg"].valueStringData() == kWatermarkMsg;
        });
    ASSERT_EQ(noopEntries.size(), 1);

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountWatermarksWritten), 1);
    }
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
