// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <list>

namespace mongo::replicated_fast_count {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;
using test_helpers::makeOplogEntry;
using test_helpers::makeWatermarkOplogEntry;
using test_helpers::NsAndUUID;
using test_helpers::OplogCursorMock;

CollectionSizeCount calculateOplogSizeCount(const std::list<repl::OplogEntry>& entries) {
    CollectionSizeCount result;
    for (const auto& entry : entries) {
        result.size += entry.getEntry().toBSON().objsize();
        result.count += 1;
    }
    return result;
}

TEST(SizeCountCheckpointBufferTest, EmptyBufferStartsWithoutWork) {
    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

// Batches are only ever cut at no-op watermarks. A scan without one never produces a batch.
TEST(SizeCountCheckpointBufferTest, ScanWithoutWatermarkNeverCutsBatch) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25)});

    buffer.scanToNoHolesEOF(cursor);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, WatermarkCutsBatch) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25),
        makeWatermarkOplogEntry(Timestamp(3, 4))};
    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    OplogCursorMock cursor(entries);

    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 25, .count = 1}}}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        // The watermark terminates the batch, so its timestamp is the batch's lastTimestamp.
        .lastTimestamp = Timestamp(3, 4)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, MultipleScansAccumulateIntoOneCheckout) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    {
        OplogCursorMock cursor(
            {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
        buffer.scanToNoHolesEOF(cursor);
    }
    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
        makeWatermarkOplogEntry(Timestamp(2, 3))};
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(2, 3)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, PendingAccumulatesEntriesWhileInFlightHasBatch) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};
    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    // Accumulate first batch and check it out, but do not acknowledge it yet.
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25),
            makeWatermarkOplogEntry(Timestamp(3, 4))};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);

        const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
        ASSERT_TRUE(checkedOutBuffer.has_value());

        const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
        const OplogScanResult expectedCheckedOutBuffer{
            .deltas =
                ReplicatedMetadataDeltas{
                    {coll.uuid,
                     ReplicatedMetadataDelta{
                         .metadata = {.sizeCount = CollectionSizeCount{.size = 25, .count = 1}}}},
                    {oplogUuid,
                     ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
            .lastTimestamp = Timestamp(3, 4)};

        EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
    }

    // Write another entry, which should be accumulated in scanToNoHolesEOF().
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25),
            makeWatermarkOplogEntry(Timestamp(3, 4)),
            makeOplogEntry(Timestamp(4, 4), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/14)};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);

        // Acknowledge previous batch so checkout can return the new write.
        buffer.acknowledgeFlush();
    }

    // Scan to watermark, which cuts the next batch, then check it out.
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(3, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/25),
            makeWatermarkOplogEntry(Timestamp(3, 4)),
            makeOplogEntry(Timestamp(4, 4), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/14),
            makeWatermarkOplogEntry(Timestamp(4, 5))};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);
        const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
        ASSERT_TRUE(checkedOutBuffer.has_value());

        // The scan above accumulates the last two elements in `entries`.
        const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(
            std::list<repl::OplogEntry>{*std::prev(entries.end(), 2), entries.back()});
        const OplogScanResult expectedCheckedOutBuffer{
            .deltas =
                ReplicatedMetadataDeltas{
                    {coll.uuid,
                     ReplicatedMetadataDelta{
                         .metadata = {.sizeCount = CollectionSizeCount{.size = 14, .count = 1}}}},
                    {oplogUuid,
                     ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
            .lastTimestamp = Timestamp(4, 5)};

        EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
    }
}

TEST(SizeCountCheckpointBufferTest, InFlightBatchIsRetriedUntilAcknowledged) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(6, 6), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/40),
        makeWatermarkOplogEntry(Timestamp(6, 7))};
    OplogCursorMock cursor(entries);

    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> first = buffer.checkoutForFlush();
    ASSERT_TRUE(first.has_value());
    const boost::optional<OplogScanResult> retried = buffer.checkoutForFlush();
    ASSERT_TRUE(retried.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 40, .count = 1}}}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(6, 7)};

    EXPECT_EQ(retried, expectedCheckedOutBuffer);
    EXPECT_EQ(first, retried);
}

TEST(SizeCountCheckpointBufferTest, AcknowledgeFlushSuccessClearsInFlight) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
         makeWatermarkOplogEntry(Timestamp(2, 3))});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_TRUE(buffer.checkoutForFlush().has_value());

    buffer.acknowledgeFlush();

    // Pending was reset when the batch was cut, so there is nothing left to flush.
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, ScanAfterAcknowledgementIsIndependent) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    // First scan.
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
            makeWatermarkOplogEntry(Timestamp(2, 2))};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);

        const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
        ASSERT_TRUE(checkedOutBuffer.has_value());

        const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
        const OplogScanResult expectedCheckedOutBuffer{
            .deltas =
                ReplicatedMetadataDeltas{
                    {coll.uuid,
                     ReplicatedMetadataDelta{
                         .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}},
                    {oplogUuid,
                     ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
            .lastTimestamp = Timestamp(2, 2)};

        EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
    }

    buffer.acknowledgeFlush();

    // Second scan.
    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
            makeWatermarkOplogEntry(Timestamp(2, 2)),
            makeOplogEntry(Timestamp(3, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
            makeWatermarkOplogEntry(Timestamp(3, 2))};
        OplogCursorMock cursor(entries);

        buffer.scanToNoHolesEOF(cursor);

        const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
        ASSERT_TRUE(checkedOutBuffer.has_value());

        const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(
            std::list<repl::OplogEntry>{*(std::next(entries.begin(), 2)), entries.back()});
        const OplogScanResult expectedCheckedOutBuffer{
            .deltas =
                ReplicatedMetadataDeltas{
                    {coll.uuid,
                     ReplicatedMetadataDelta{
                         .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 1}}}},
                    {oplogUuid,
                     ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
            .lastTimestamp = Timestamp(3, 2)};

        EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
    }
}

// A watermark seen while a batch is already in flight is folded into _pending like any other
// record, and the in-flight batch is left untouched.
TEST(SizeCountCheckpointBufferTest, WatermarkWithExistingInFlightFoldsIntoPending) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
            makeWatermarkOplogEntry(Timestamp(2, 2)),
            makeOplogEntry(Timestamp(2, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
            makeWatermarkOplogEntry(Timestamp(2, 4))};

        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);

        // The first watermark cut the first batch, and the second was folded into _pending.
        const boost::optional<OplogScanResult> batch = buffer.checkoutForFlush();
        ASSERT_TRUE(batch.has_value());
        EXPECT_EQ(batch->deltas.at(coll.uuid).metadata.sizeCount,
                  (CollectionSizeCount{.size = 10, .count = 1}));
        EXPECT_EQ(batch->lastTimestamp, Timestamp(2, 2));

        buffer.acknowledgeFlush();
    }

    {
        const std::list<repl::OplogEntry> entries{
            makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
            makeWatermarkOplogEntry(Timestamp(2, 2)),
            makeOplogEntry(Timestamp(2, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
            makeWatermarkOplogEntry(Timestamp(2, 4)),
            makeOplogEntry(Timestamp(2, 5), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30),
            makeWatermarkOplogEntry(Timestamp(2, 6))};

        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);

        // The second and third watermark are combined into _pending, so we lose the second
        // watermark.
        // TODO(SERVER-135085): Is this the behavior we want?
        const boost::optional<OplogScanResult> batch = buffer.checkoutForFlush();
        ASSERT_TRUE(batch.has_value());
        EXPECT_EQ(batch->deltas.at(coll.uuid).metadata.sizeCount,
                  (CollectionSizeCount{.size = 50, .count = 2}));
        EXPECT_EQ(batch->lastTimestamp, Timestamp(2, 6));
    }
}

TEST(SizeCountCheckpointBufferTest, InternalOnlyScanProducesNoFlushableWork) {
    const UUID oplogUuid = UUID::gen();
    const repl::OplogEntry entry = test_helpers::makeContainerOplogEntry(
        Timestamp(2, 1), ident::kFastCountMetadataStore, repl::OpTypeEnum::kContainerInsert);

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    OplogCursorMock cursor({entry});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, DropThenImportAcrossScansYieldsDroppedAndRecreated) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("test", "collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);
    {
        OplogCursorMock cursor({test_helpers::makeDropOplogEntry(Timestamp(2, 1), coll)});
        buffer.scanToNoHolesEOF(cursor);
    }
    const std::list<repl::OplogEntry> entries{
        test_helpers::makeDropOplogEntry(Timestamp(2, 1), coll),
        test_helpers::makeImportCollectionOplogEntry(
            Timestamp(2, 2), coll, /*numRecords=*/3, /*dataSize=*/90),
        makeWatermarkOplogEntry(Timestamp(2, 3))};
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 90, .count = 3}},
                     .state = DDLState::kDroppedAndRecreated}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(2, 3)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, CreateThenDropWithinScanCancelsOut) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("test", "collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogCursorMock cursor({test_helpers::makeCreateOplogEntry(Timestamp(2, 1), coll),
                            test_helpers::makeDropOplogEntry(Timestamp(2, 2), coll),
                            makeWatermarkOplogEntry(Timestamp(2, 3))});
    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    // The create and drop cancel out, so the collection delta is gone.
    EXPECT_FALSE(checkedOutBuffer->deltas.contains(coll.uuid));
    ASSERT_TRUE(checkedOutBuffer->lastTimestamp.has_value());
    EXPECT_EQ(*checkedOutBuffer->lastTimestamp, Timestamp(2, 3));
}

TEST(SizeCountCheckpointBufferTest, PartialScanThenWriteConflictDoesNotDoubleCount) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
        makeOplogEntry(Timestamp(2, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30),
        makeWatermarkOplogEntry(Timestamp(2, 4)),
    };

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    // The first scan fails after consuming one entry.
    {
        OplogCursorMock cursor(entries, /*throwWriteConflictOnNthCall=*/2);
        ASSERT_THROWS_CODE(buffer.scanToNoHolesEOF(cursor), DBException, ErrorCodes::WriteConflict);
    }

    // The second scan reads the rest of the entries and succeeds.
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 60, .count = 3}}}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(2, 4)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

// A scan that resumes after a mid-scan write conflict folds each entry's hash exactly once. XOR is
// its own inverse, so re-consuming an already-buffered record would cancel its contribution rather
// than produce an obviously wrong value. The three hashes overlap in their set bits, so the
// expectation does not also hold for a sum or a bitwise or.
TEST(SizeCountCheckpointBufferTest, PartialScanThenWriteConflictFoldsEachHashOnce) {
    constexpr int64_t kHashA = 0x0123456789ABCDEF;
    constexpr int64_t kHashB = static_cast<int64_t>(0xF0E1D2C3B4A59687);
    constexpr int64_t kHashC = 0x00FF00FF00FF00FF;

    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10, kHashA),
        makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20, kHashB),
        makeOplogEntry(Timestamp(2, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30, kHashC),
        makeWatermarkOplogEntry(Timestamp(2, 4)),
    };

    SizeCountCheckpointBuffer buffer(oplogUuid, boost::none);

    // The first scan fails after consuming one entry, so the resumed scan must skip that entry's
    // hash rather than fold it a second time.
    {
        OplogCursorMock cursor(entries, /*throwWriteConflictOnNthCall=*/2);
        ASSERT_THROWS_CODE(buffer.scanToNoHolesEOF(cursor), DBException, ErrorCodes::WriteConflict);
    }
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    ASSERT_TRUE(checkedOutBuffer->deltas.contains(coll.uuid));
    EXPECT_EQ(
        checkedOutBuffer->deltas.at(coll.uuid).metadata,
        (CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 60, .count = 3},
                                      .hash = kHashA ^ kHashB ^ kHashC}));
}

TEST(SizeCountCheckpointBufferTest, SeekExactDoesNotDoubleCountLastBufferedRid) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const RecordId lastBufferedRid(Timestamp(2, 2).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);
    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(2, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
        makeOplogEntry(Timestamp(2, 3), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30),
        makeWatermarkOplogEntry(Timestamp(2, 4))};
    OplogCursorMock cursor(entries);
    buffer.scanToNoHolesEOF(cursor);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(
        std::list<repl::OplogEntry>{*(std::next(entries.begin(), 2)), entries.back()});
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 1}}}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(2, 4)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, LostLastBufferedRidResumesFromNextEntry) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const RecordId lastBufferedRid(Timestamp(2, 1).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(1, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100),
        makeOplogEntry(Timestamp(5, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(5, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
        makeWatermarkOplogEntry(Timestamp(5, 3))};

    {
        OplogCursorMock cursor(entries);
        ASSERT_TASSERT_CODE(buffer.scanToNoHolesEOF(cursor), 12101812);
    }
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    // The follow-up scan resumes at (5, 1) and consumes up through the watermark at (5, 3).
    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(
        std::list<repl::OplogEntry>{std::next(entries.begin(), 1), entries.end()});
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {coll.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}},
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(5, 3)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, LostLastBufferedRidResumesOntoWatermarkCutsBatch) {
    const UUID oplogUuid = UUID::gen();
    const RecordId lastBufferedRid(Timestamp(2, 1).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);

    // The only entry in the oplog is a watermark, so after successfully resuming, the in-flight
    // batch should be immediately cut.
    const std::list<repl::OplogEntry> entries{makeWatermarkOplogEntry(Timestamp(2, 2))};

    {
        OplogCursorMock cursor(entries);
        ASSERT_TASSERT_CODE(buffer.scanToNoHolesEOF(cursor), 12101812);
    }
    {
        OplogCursorMock cursor(entries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());

    const CollectionSizeCount expectedOplogSizeCount = calculateOplogSizeCount(entries);
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {oplogUuid,
                 ReplicatedMetadataDelta{.metadata = {.sizeCount = expectedOplogSizeCount}}}},
        .lastTimestamp = Timestamp(2, 2)};

    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);
}

TEST(SizeCountCheckpointBufferTest, RepeatedLostLastBufferedRidTassertsOncePerEpisode) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const RecordId lastBufferedRid(Timestamp(2, 1).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);

    const std::list<repl::OplogEntry> episodeOneEntries{
        makeOplogEntry(Timestamp(5, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(5, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20),
        makeWatermarkOplogEntry(Timestamp(5, 3))};
    const std::list<repl::OplogEntry> episodeTwoEntries{
        makeOplogEntry(Timestamp(6, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30),
        makeOplogEntry(Timestamp(6, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/40),
        makeWatermarkOplogEntry(Timestamp(6, 3))};

    // Episode one: the (2, 1) start point is lost.
    {
        OplogCursorMock cursor(episodeOneEntries);
        ASSERT_TASSERT_CODE(buffer.scanToNoHolesEOF(cursor), 12101812);
    }
    {
        OplogCursorMock cursor(episodeOneEntries);
        buffer.scanToNoHolesEOF(cursor);
    }
    // The watermark at (5, 3) cut the first batch. Acknowledge it before the next episode.
    const boost::optional<OplogScanResult> firstBatch = buffer.checkoutForFlush();
    ASSERT_TRUE(firstBatch.has_value());
    EXPECT_EQ(firstBatch->deltas.at(coll.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 30, .count = 2}));
    EXPECT_EQ(firstBatch->lastTimestamp, Timestamp(5, 3));
    buffer.acknowledgeFlush();

    // Episode two: the recovered start point (5, 3) is lost again.
    {
        OplogCursorMock cursor(episodeTwoEntries);
        ASSERT_TASSERT_CODE(buffer.scanToNoHolesEOF(cursor), 12101812);
    }
    {
        OplogCursorMock cursor(episodeTwoEntries);
        buffer.scanToNoHolesEOF(cursor);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    EXPECT_EQ(checkedOutBuffer->deltas.at(coll.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 70, .count = 2}));
    ASSERT_TRUE(checkedOutBuffer->lastTimestamp.has_value());
    EXPECT_EQ(*checkedOutBuffer->lastTimestamp, Timestamp(6, 3));
}

TEST(SizeCountCheckpointBufferTest, ReadyForWatermarkIsFalseOnFreshBuffer) {
    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);

    // The pending accumulator has consumed nothing, so there is no work to watermark.
    EXPECT_FALSE(buffer.readyForWatermark());
}

TEST(SizeCountCheckpointBufferTest, ReadyForWatermarkIsTrueWithPendingWorkAndNoInFlightBatch) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_TRUE(buffer.readyForWatermark());
}

TEST(SizeCountCheckpointBufferTest, ReadyForWatermarkIsFalseWhenInFlightBatchExists) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
         makeWatermarkOplogEntry(Timestamp(2, 2))});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_FALSE(buffer.readyForWatermark());
}

// The blocking await needs an OperationContext for interruption support.
class SizeCountCheckpointBufferAwaitTest : public CatalogTestFixture {};

TEST_F(SizeCountCheckpointBufferAwaitTest, AwaitReturnsImmediatelyWhenInFlightBatchExists) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
         makeWatermarkOplogEntry(Timestamp(2, 2))});
    buffer.scanToNoHolesEOF(cursor);

    // The batch was already cut by the scan, so the await returns without waiting.
    const OplogScanResult batch = buffer.awaitCheckoutForFlush(operationContext());
    EXPECT_EQ(batch.lastTimestamp, Timestamp(2, 2));
}

TEST_F(SizeCountCheckpointBufferAwaitTest, AwaitBlocksWhenNoBatchExists) {
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogScanResult batch;
    stdx::thread waiter([&]() { batch = buffer.awaitCheckoutForFlush(operationContext()); });

    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
         makeWatermarkOplogEntry(Timestamp(2, 2))});
    buffer.scanToNoHolesEOF(cursor);

    waiter.join();
    EXPECT_EQ(batch.lastTimestamp, Timestamp(2, 2));
}

TEST_F(SizeCountCheckpointBufferAwaitTest, AwaitRecordsElapsedWaitTime) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    SizeCountCheckpointBuffer buffer(UUID::gen(), boost::none);
    OplogScanResult batch;
    stdx::thread waiter([&]() { batch = buffer.awaitCheckoutForFlush(operationContext()); });

    // Delay the batch cut so the waiter is guaranteed to block for at least this long.
    constexpr auto kWaitTime = Milliseconds(100);
    sleepFor(kWaitTime);

    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
         makeWatermarkOplogEntry(Timestamp(2, 2))});
    buffer.scanToNoHolesEOF(cursor);

    waiter.join();
    EXPECT_EQ(batch.lastTimestamp, Timestamp(2, 2));
    EXPECT_GE(capturer.readInt64Counter(MetricNames::kReplicatedFastCountWatermarkAwaitTimeMsTotal),
              kWaitTime.count());
}

DEATH_TEST(SizeCountCheckpointBufferDeathTest, LastBufferedRidBeforeEntries, "12101812") {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const RecordId lastBufferedRid(Timestamp(2, 1).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);

    const std::list<repl::OplogEntry> entries{
        makeOplogEntry(Timestamp(5, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10),
        makeOplogEntry(Timestamp(5, 2), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20)};
    OplogCursorMock cursor(entries);

    buffer.scanToNoHolesEOF(cursor);
}

TEST(SizeCountCheckpointBufferTest, LastBufferedRidEqualToEntry) {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const RecordId lastBufferedRid(Timestamp(2, 1).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);

    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(2, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
    buffer.scanToNoHolesEOF(cursor);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

DEATH_TEST(SizeCountCheckpointBufferDeathTest, LastBufferedRidAfterEntry, "12101812") {
    const UUID oplogUuid = UUID::gen();
    const NsAndUUID coll{.nss = NamespaceString::createNamespaceString_forTest("collA"),
                         .uuid = UUID::gen()};

    const RecordId lastBufferedRid(Timestamp(2, 1).asULL());
    SizeCountCheckpointBuffer buffer(oplogUuid, lastBufferedRid);

    OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(1, 1), coll, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
    buffer.scanToNoHolesEOF(cursor);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
