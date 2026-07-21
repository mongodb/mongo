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
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

namespace mongo::replicated_fast_count::oplog_tailer {
namespace {

using test_helpers::makeOplogEntry;
using test_helpers::NsAndUUID;
using test_helpers::scanForAccurateSizeCount;
using test_helpers::writeToOplog;

class OplogTailerTest : public CatalogTestFixture {
public:
    OplogTailerTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
    }

    UUID oplogUuid() const {
        AutoGetOplogFastPath oplogRead(_opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->uuid();
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
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));

    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    bufferNewOplogEntries(_opCtx, buffer);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            SizeCountDeltas{
                {_collA.uuid,
                 SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}},
                {oplogUuid(),
                 SizeCountDelta{.sizeCount = scanForAccurateSizeCount(
                                    _opCtx, NamespaceString::kRsOplogNamespace)}}},
        .lastTimestamp = Timestamp(1, 2)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);

    buffer.acknowledgeFlushSuccess();
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST_F(OplogTailerTest, ScanFromBeginningOnEmptyOplogMakesNoProgress) {
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);
    bufferNewOplogEntries(_opCtx, buffer);
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
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
    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);

    // First scan sees one entry.
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));

    bufferNewOplogEntries(_opCtx, buffer);

    // Second scan sees two entries.
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 3), _collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/7));

    bufferNewOplogEntries(_opCtx, buffer);

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    ASSERT_TRUE(checkedOutBuffer.has_value());
    EXPECT_EQ(checkedOutBuffer->lastTimestamp, Timestamp(1, 3));
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            SizeCountDeltas{
                {_collA.uuid,
                 SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}},
                {_collB.uuid,
                 SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 7, .count = 1}}},
                {oplogUuid(),
                 SizeCountDelta{.sizeCount = scanForAccurateSizeCount(
                                    _opCtx, NamespaceString::kRsOplogNamespace)}}},
        .lastTimestamp = Timestamp(1, 3)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);

    buffer.acknowledgeFlushSuccess();
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
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 1), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    writeToOplog(
        _opCtx,
        makeOplogEntry(Timestamp(1, 2), _collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));

    SizeCountCheckpointBuffer buffer(oplogUuid(), /*lastBufferedRid=*/boost::none);

    {
        // Make bufferNewOplogEntries() fail twice before successfully scanning the oplog.
        FailPointEnableBlock fp("WTWriteConflictExceptionForReads",
                                FailPoint::ModeOptions{.mode = FailPoint::nTimes, .val = 2});
        bufferNewOplogEntries(_opCtx, buffer);
    }

    const boost::optional<OplogScanResult> checkedOutBuffer = buffer.checkoutForFlush();
    const OplogScanResult expectedCheckedOutBuffer{
        .deltas =
            SizeCountDeltas{
                {_collA.uuid,
                 SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}},
                {oplogUuid(),
                 SizeCountDelta{.sizeCount = scanForAccurateSizeCount(
                                    _opCtx, NamespaceString::kRsOplogNamespace)}}},
        .lastTimestamp = Timestamp(1, 2)};
    EXPECT_EQ(checkedOutBuffer, expectedCheckedOutBuffer);

    buffer.acknowledgeFlushSuccess();
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST_F(OplogTailerTest, LastBufferedRidNotFound) {
    unittest::ServerParameterGuard featureFlag("featureFlagSizeBasedOplogTruncationForDisagg",
                                               false);
    SizeCountCheckpointBuffer buffer(oplogUuid(), RecordId(1));
    bufferNewOplogEntries(_opCtx, buffer);
}

using OplogTailerDeathTest = OplogTailerTest;

DEATH_TEST_F(OplogTailerDeathTest, LastBufferedRidNotFound, "12101812") {
    unittest::ServerParameterGuard featureFlag("featureFlagSizeBasedOplogTruncationForDisagg",
                                               true);
    SizeCountCheckpointBuffer buffer(oplogUuid(), RecordId(1));
    bufferNewOplogEntries(_opCtx, buffer);
}

}  // namespace
}  // namespace mongo::replicated_fast_count::oplog_tailer
