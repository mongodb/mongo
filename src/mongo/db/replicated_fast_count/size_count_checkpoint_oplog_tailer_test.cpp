// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"

#include "mongo/db/operation_context_group.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_store_write_conflict_fail_points.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

#include <list>
#include <utility>

namespace mongo::replicated_fast_count::oplog_tailer {
namespace {

using test_helpers::makeOplogEntry;
using test_helpers::makeWatermarkOplogEntry;
using test_helpers::NsAndUUID;
using test_helpers::scanForAccurateSizeCount;
using test_helpers::writeToOplog;

class OplogTailerTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>());
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
    }

    UUID oplogUuid() const {
        AutoGetOplogFastPath oplogRead(_opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->uuid();
    }

    OplogScanResult bufferEntries(std::list<repl::OplogEntry> entries, Timestamp watermarkTs) {
        entries.push_back(makeWatermarkOplogEntry(watermarkTs));
        test_helpers::OplogCursorMock cursor(std::move(entries));
        SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
        buffer.scanToNoHolesEOF(cursor);
        auto batch = buffer.checkoutForFlush();
        invariant(batch);
        EXPECT_EQ(batch->lastTimestamp, watermarkTs);
        return *batch;
    }

    void assertTruncateRangeDelta(const NsAndUUID& target, CollectionSizeCount expectedDelta) {
        const auto oplogId = oplogUuid();
        writeToOplog(_opCtx,
                     test_helpers::makeTruncateRangeOplogEntry(
                         Timestamp(1, 1), target, /*bytesDeleted=*/150, /*docsDeleted=*/4));
        writeToOplog(_opCtx, makeWatermarkOplogEntry(Timestamp(1, 2)));

        SizeCountCheckpointBuffer buffer(oplogId, /*lastBufferedRid=*/boost::none);
        bufferNewOplogEntries(_opCtx, buffer);

        const auto result = buffer.checkoutForFlush();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->lastTimestamp, Timestamp(1, 2));
        ASSERT_EQ(result->deltas.size(), 1);
        ASSERT_TRUE(result->deltas.contains(oplogId));
        const auto expectedSizeCount =
            scanForAccurateSizeCount(_opCtx, NamespaceString::kRsOplogNamespace) + expectedDelta;
        EXPECT_EQ(result->deltas.at(oplogId).metadata.sizeCount, expectedSizeCount);
        EXPECT_FALSE(result->deltas.at(oplogId).metadata.hash);
    }

    OperationContext* _opCtx = nullptr;
    const NsAndUUID _collA{
        .nss = NamespaceString::createNamespaceString_forTest("tailer_test", "collA"),
        .uuid = UUID::gen(),
    };
    const NsAndUUID _collB{
        .nss = NamespaceString::createNamespaceString_forTest("tailer_test", "collB"),
        .uuid = UUID::gen(),
    };
};

TEST_F(OplogTailerTest, ScanFromBeginningAccountsAllVisibleEntries) {
    otel::metrics::OtelMetricsCapturer capturer;

    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));
    writeToOplog(_opCtx, makeWatermarkOplogEntry(Timestamp(1, 3)));

    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    bufferNewOplogEntries(_opCtx, buffer);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {_collA.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}},
                {oplogUuid(),
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = scanForAccurateSizeCount(
                                      _opCtx, NamespaceString::kRsOplogNamespace)}}}},
        .lastTimestamp = Timestamp(1, 3)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);

    buffer.acknowledgeFlush();
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kReplicatedFastCountTailerWatermarksSeen),
                  1);
    }
}

TEST_F(OplogTailerTest, ScanFromBeginningOnEmptyOplogMakesNoProgress) {
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    bufferNewOplogEntries(_opCtx, buffer);
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST_F(OplogTailerTest, InternalOnlyEntriesDoNotProducePendingWork) {
    for (const auto containerIdent :
         {ident::kFastCountMetadataStore, ident::kFastCountMetadataStoreTimestamps}) {
        test_helpers::OplogCursorMock cursor({test_helpers::makeContainerOplogEntry(
            Timestamp(1, 1), containerIdent, repl::OpTypeEnum::kContainerUpdate)});
        SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
        buffer.scanToNoHolesEOF(cursor);
        EXPECT_FALSE(buffer.readyForWatermark());
        EXPECT_FALSE(buffer.checkoutForFlush());
    }

    const repl::OplogEntry internalApplyOps = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp(1, 1), 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
        .oField =
            BSON("applyOps" << BSON_ARRAY(
                     BSON("op" << "cu" << "container" << ident::kFastCountMetadataStore << "o"
                               << BSON("k" << 1LL))
                     << BSON("op" << "cu" << "container" << ident::kFastCountMetadataStoreTimestamps
                                  << "o" << BSON("k" << 1LL)))),
        .wallClockTime = Date_t::now()}};
    test_helpers::OplogCursorMock cursor({internalApplyOps});
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    buffer.scanToNoHolesEOF(cursor);
    EXPECT_FALSE(buffer.readyForWatermark());
    EXPECT_FALSE(buffer.checkoutForFlush());
}

TEST_F(OplogTailerTest, InternalEntriesAfterWatermarkDoNotChangeBatchCutoff) {
    test_helpers::OplogCursorMock cursor(
        {makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 10),
         makeWatermarkOplogEntry(Timestamp(1, 2)),
         test_helpers::makeContainerOplogEntry(Timestamp(1, 3),
                                               ident::kFastCountMetadataStoreTimestamps,
                                               repl::OpTypeEnum::kContainerUpdate)});
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    buffer.scanToNoHolesEOF(cursor);
    const auto batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->lastTimestamp, Timestamp(1, 2));
    EXPECT_EQ(batch->deltas.at(_collA.uuid).metadata.sizeCount, (CollectionSizeCount{10, 1}));
    buffer.acknowledgeFlush();
    EXPECT_FALSE(buffer.readyForWatermark());
    EXPECT_FALSE(buffer.checkoutForFlush());
}

TEST_F(OplogTailerTest, NoopProducesOnlyOplogMetadataDelta) {
    const auto batch = bufferEntries(
        {makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kNoop)}, Timestamp(1, 2));
    EXPECT_EQ(batch.deltas.size(), 1);
    EXPECT_FALSE(batch.deltas.contains(_collA.uuid));
    EXPECT_EQ(batch.deltas.at(oplogUuid()).metadata.sizeCount.count, 2);
}

TEST_F(OplogTailerTest, CreateAndInsertsInApplyOpsUseWatermarkCutoff) {
    int increment = 1;
    for (const bool includeInserts : {false, true}) {
        const auto coll = includeInserts ? _collB : _collA;
        BSONArrayBuilder innerOps;
        innerOps.append(BSON("op" << "c" << "ns" << coll.nss.getCommandNS().ns_forTest() << "ui"
                                  << coll.uuid << "o" << BSON("create" << coll.nss.coll())));
        if (includeInserts) {
            for (const auto size : {10, 20}) {
                innerOps.append(BSON("op" << "i" << "ns" << coll.nss.ns_forTest() << "ui"
                                          << coll.uuid << "o" << BSON("_id" << size) << "m"
                                          << BSON("sz" << size)));
            }
        }
        const Timestamp ts(1, increment++);
        const repl::OplogEntry entry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
            .oField = BSON("applyOps" << innerOps.arr()),
            .wallClockTime = Date_t::now()}};
        const auto batch = bufferEntries({entry}, Timestamp(2, 1));
        const auto& delta = batch.deltas.at(coll.uuid);
        EXPECT_EQ(delta.state, DDLState::kCreated);
        EXPECT_EQ(delta.metadata.sizeCount,
                  (includeInserts ? CollectionSizeCount{30, 2} : CollectionSizeCount{0, 0}));
    }
}

TEST_F(OplogTailerTest, DropCreateFromMigrateResetsAccumulatedTotals) {
    const repl::OplogEntry createFromMigrate = repl::DurableOplogEntry{
        repl::DurableOplogEntryParams{.opTime = repl::OpTime(Timestamp(1, 4), 1),
                                      .opType = repl::OpTypeEnum::kCommand,
                                      .nss = _collA.nss.getCommandNS(),
                                      .uuid = _collA.uuid,
                                      .fromMigrate = true,
                                      .oField = BSON("create" << _collA.nss.coll()),
                                      .wallClockTime = Date_t::now()}};
    const auto batch =
        bufferEntries({makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 100),
                       test_helpers::makeDropOplogEntry(Timestamp(1, 3), _collA),
                       createFromMigrate,
                       makeOplogEntry(Timestamp(1, 5), _collA, repl::OpTypeEnum::kInsert, 200)},
                      Timestamp(1, 6));
    const auto& delta = batch.deltas.at(_collA.uuid);
    EXPECT_EQ(delta.state, DDLState::kDroppedAndRecreated);
    EXPECT_EQ(delta.metadata.sizeCount, (CollectionSizeCount{200, 1}));
}

TEST_F(OplogTailerTest, CreateAndDropCancelCollectionDelta) {
    const auto batch = bufferEntries({test_helpers::makeCreateOplogEntry(Timestamp(1, 1), _collA),
                                      test_helpers::makeDropOplogEntry(Timestamp(1, 2), _collA)},
                                     Timestamp(1, 3));
    EXPECT_FALSE(batch.deltas.contains(_collA.uuid));
    EXPECT_EQ(batch.deltas.size(), 1);
    EXPECT_EQ(batch.deltas.at(oplogUuid()).metadata.sizeCount.count, 3);
}

TEST_F(OplogTailerTest, MixedOpsWithTruncateRangeAccumulatePerCollection) {
    const auto batch =
        bufferEntries({test_helpers::makeTruncateRangeOplogEntry(Timestamp(1, 1), _collA, 120, 3),
                       makeOplogEntry(Timestamp(1, 2), _collB, repl::OpTypeEnum::kInsert, 40),
                       makeOplogEntry(Timestamp(1, 3), _collB, repl::OpTypeEnum::kInsert, 60),
                       makeOplogEntry(Timestamp(1, 4), _collB, repl::OpTypeEnum::kInsert, 50),
                       makeOplogEntry(Timestamp(1, 5), _collB, repl::OpTypeEnum::kUpdate, -10),
                       makeOplogEntry(Timestamp(1, 6), _collB, repl::OpTypeEnum::kDelete, -40),
                       test_helpers::makeTruncateRangeOplogEntry(Timestamp(1, 7), _collB, 60, 2)},
                      Timestamp(1, 8));
    // Non-oplog truncations are ignored; only the CRUD operations contribute user deltas.
    EXPECT_FALSE(batch.deltas.contains(_collA.uuid));
    ASSERT_TRUE(batch.deltas.contains(_collB.uuid));
    EXPECT_EQ(batch.deltas.at(_collB.uuid).metadata.sizeCount, (CollectionSizeCount{100, 2}));
    const auto insertThenTruncate =
        bufferEntries({makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 50),
                       makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, 50),
                       test_helpers::makeTruncateRangeOplogEntry(Timestamp(1, 3), _collA, 30, 1)},
                      Timestamp(1, 4));
    ASSERT_TRUE(insertThenTruncate.deltas.contains(_collA.uuid));
    EXPECT_EQ(insertThenTruncate.deltas.at(_collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{100, 2}));
}

TEST_F(OplogTailerTest, TruncateRangeInApplyOpsAndNestedApplyOps) {
    for (const bool nested : {false, true}) {
        const auto coll = nested ? _collB : _collA;
        const auto truncate =
            test_helpers::makeTruncateRangeOplogEntry(Timestamp(1, 1), coll, 90, 2);
        const auto adminCmd = NamespaceString::createNamespaceString_forTest("admin", "$cmd");
        BSONObj inner = BSON("op" << "c" << "ns" << coll.nss.getCommandNS().ns_forTest() << "ui"
                                  << coll.uuid << "o" << truncate.getObject());
        if (nested) {
            inner = BSON("op" << "c" << "ns" << adminCmd.ns_forTest() << "o"
                              << BSON("applyOps" << BSON_ARRAY(inner)));
        }
        const repl::OplogEntry entry = repl::DurableOplogEntry{
            repl::DurableOplogEntryParams{.opTime = repl::OpTime(Timestamp(1, 1), 1),
                                          .opType = repl::OpTypeEnum::kCommand,
                                          .nss = adminCmd,
                                          .oField = BSON("applyOps" << BSON_ARRAY(inner)),
                                          .wallClockTime = Date_t::now()}};
        const auto batch = bufferEntries({entry}, Timestamp(1, 2));
        // Wrapping a non-oplog truncation in applyOps does not make it countable.
        EXPECT_FALSE(batch.deltas.contains(coll.uuid));
        ASSERT_EQ(batch.deltas.size(), 1);
        EXPECT_EQ(batch.deltas.at(oplogUuid()).metadata.sizeCount.count, 2);
    }
}

TEST_F(OplogTailerTest, OplogAccountingAcrossBatches) {
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    const auto insert = makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 50);
    const auto watermark1 = makeWatermarkOplogEntry(Timestamp(1, 2));
    writeToOplog(_opCtx, insert);
    writeToOplog(_opCtx, watermark1);
    bufferNewOplogEntries(_opCtx, buffer);
    const auto first = buffer.checkoutForFlush();
    ASSERT_TRUE(first);
    const int64_t firstSize = static_cast<int64_t>(insert.getEntry().toBSON().objsize()) +
        watermark1.getEntry().toBSON().objsize();
    EXPECT_EQ(first->deltas.at(oplogUuid()).metadata.sizeCount,
              (CollectionSizeCount{firstSize, 2}));
    EXPECT_EQ(first->deltas.at(_collA.uuid).metadata.sizeCount, (CollectionSizeCount{50, 1}));
    buffer.acknowledgeFlush();

    const auto update = makeOplogEntry(Timestamp(1, 3), _collA, repl::OpTypeEnum::kUpdate, 50);
    const auto watermark2 = makeWatermarkOplogEntry(Timestamp(1, 4));
    writeToOplog(_opCtx, update);
    writeToOplog(_opCtx, watermark2);
    bufferNewOplogEntries(_opCtx, buffer);
    const auto second = buffer.checkoutForFlush();
    ASSERT_TRUE(second);
    const int64_t secondSize = static_cast<int64_t>(update.getEntry().toBSON().objsize()) +
        watermark2.getEntry().toBSON().objsize();
    EXPECT_EQ(second->lastTimestamp, Timestamp(1, 4));
    EXPECT_EQ(second->deltas.at(oplogUuid()).metadata.sizeCount,
              (CollectionSizeCount{secondSize, 2}));
    EXPECT_EQ(second->deltas.at(_collA.uuid).metadata.sizeCount, (CollectionSizeCount{50, 0}));
    buffer.acknowledgeFlush();
    bufferNewOplogEntries(_opCtx, buffer);
    EXPECT_FALSE(buffer.checkoutForFlush());
}

TEST_F(OplogTailerTest, UserWritesAndOplogTruncationAccumulateTogether) {
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    const auto insert = makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, 20);
    const NsAndUUID oplogColl{.nss = NamespaceString::kRsOplogNamespace, .uuid = oplogUuid()};
    const auto truncate = test_helpers::makeTruncateRangeOplogEntry(
        Timestamp(1, 2), oplogColl, /*bytesDeleted=*/100, /*docsDeleted=*/1);
    const auto watermark = makeWatermarkOplogEntry(Timestamp(1, 3));
    writeToOplog(_opCtx, insert);
    writeToOplog(_opCtx, truncate);
    writeToOplog(_opCtx, watermark);
    bufferNewOplogEntries(_opCtx, buffer);
    const auto batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->lastTimestamp, Timestamp(1, 3));
    const int64_t expectedSize = static_cast<int64_t>(insert.getEntry().toBSON().objsize()) +
        truncate.getEntry().toBSON().objsize() + watermark.getEntry().toBSON().objsize() - 100;
    EXPECT_EQ(batch->deltas.at(oplogUuid()).metadata.sizeCount,
              (CollectionSizeCount{expectedSize, 2}));
    EXPECT_EQ(batch->deltas.at(_collA.uuid).metadata.sizeCount, (CollectionSizeCount{20, 1}));
}

TEST_F(OplogTailerTest, PreimagesTruncateRangeDoesNotContributeDeltas) {
    const NsAndUUID preimages{.nss = NamespaceString::kChangeStreamPreImagesNamespace,
                              .uuid = UUID::gen()};
    assertTruncateRangeDelta(preimages, CollectionSizeCount{.size = 0, .count = 0});
}

TEST_F(OplogTailerTest, OplogTruncateRangeContributesNegativeDeltas) {
    const NsAndUUID oplog{.nss = NamespaceString::kRsOplogNamespace, .uuid = oplogUuid()};
    assertTruncateRangeDelta(oplog, CollectionSizeCount{.size = -150, .count = -4});
}

TEST_F(OplogTailerTest, TimestampAfterLastBufferedRidDoesNotDoubleCount) {
    const Timestamp ts(1, 1);
    writeToOplog(_opCtx, makeOplogEntry(ts, _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));

    SizeCountCheckpointBuffer buffer(
        oplogUuid(), unittest::assertGet(record_id_helpers::keyForOptime(ts, KeyFormat::Long)));

    bufferNewOplogEntries(_opCtx, buffer);

    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST_F(OplogTailerTest, MultipleIterationsAccumulateInBuffer) {
    otel::metrics::OtelMetricsCapturer capturer;

    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);

    // First scan sees one entry.
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));

    bufferNewOplogEntries(_opCtx, buffer);

    // Second scan sees two more entries.
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 3), _collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/7));

    bufferNewOplogEntries(_opCtx, buffer);

    // Third scan consumes the watermark, cutting the accumulated batch.
    writeToOplog(_opCtx, makeWatermarkOplogEntry(Timestamp(1, 4)));

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kReplicatedFastCountTailerWatermarksSeen),
                  0);
    }

    bufferNewOplogEntries(_opCtx, buffer);

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kReplicatedFastCountTailerWatermarksSeen),
                  1);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    EXPECT_EQ(checkedOutBuffer->lastTimestamp, Timestamp(1, 4));
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {_collA.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}},
                {_collB.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 7, .count = 1}}}},
                {oplogUuid(),
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = scanForAccurateSizeCount(
                                      _opCtx, NamespaceString::kRsOplogNamespace)}}}},
        .lastTimestamp = Timestamp(1, 4)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);

    buffer.acknowledgeFlush();
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());

    bufferNewOplogEntries(_opCtx, buffer);
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST_F(OplogTailerTest, RunInterruptedWhileWaitingExitsPromptly) {
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));

    OperationContextGroup ocg;
    SizeCountCheckpointBuffer threadBuffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    Status threadStatus = Status::OK();
    auto [promise, future] = makePromiseFuture<void>();

    stdx::thread t([&] {
        auto client = getServiceContext()->getService()->makeClient("checkpointTailerThread");
        AlternativeClientRegion acr(client);
        auto opCtxHolder = ocg.makeOperationContext(cc());
        promise.emplaceValue();

        try {
            run(opCtxHolder, threadBuffer);
        } catch (const DBException& ex) {
            threadStatus = ex.toStatus();
        }
    });

    std::move(future).get();
    ocg.interrupt(ErrorCodes::InterruptedDueToReplStateChange);
    t.join();

    ASSERT_OK(threadStatus.code());
}

TEST_F(OplogTailerTest, RetriesScanOnWriteConflict) {
    otel::metrics::OtelMetricsCapturer capturer;

    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));
    writeToOplog(_opCtx, makeWatermarkOplogEntry(Timestamp(1, 3)));

    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);

    {
        // Make bufferNewOplogEntries() fail twice before successfully scanning the oplog.
        auto fp = enableWriteConflictForReads(
            FailPoint::ModeOptions{.mode = FailPoint::nTimes, .val = 2});
        bufferNewOplogEntries(_opCtx, buffer);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            ReplicatedMetadataDeltas{
                {_collA.uuid,
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}},
                {oplogUuid(),
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = scanForAccurateSizeCount(
                                      _opCtx, NamespaceString::kRsOplogNamespace)}}}},
        .lastTimestamp = Timestamp(1, 3)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);

    buffer.acknowledgeFlush();
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kReplicatedFastCountTailerRetriedScanCount),
                  2);
    }
}

using OplogTailerDeathTest = OplogTailerTest;

DEATH_TEST_F(OplogTailerDeathTest, LastBufferedRidNotFound, "12101812") {
    SizeCountCheckpointBuffer buffer(oplogUuid(), RecordId(1));
    bufferNewOplogEntries(_opCtx, buffer);
}

}  // namespace
}  // namespace mongo::replicated_fast_count::oplog_tailer
