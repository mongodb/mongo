// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_coordinator.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

namespace mongo::replicated_fast_count {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class SizeCountCheckpointCoordinatorTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();

        ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), _opCtx));
        ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), _opCtx));

        _coordinator = std::make_unique<SizeCountCheckpointCoordinator>(
            _sizeCountStore, _timestampStore, oplogUuid(), Timestamp::min());
    }

    UUID oplogUuid() const {
        AutoGetOplogFastPath oplogRead(_opCtx, OplogAccessMode::kRead);
        return oplogRead.getCollection()->uuid();
    }

    void tearDown() override {
        _coordinator.reset();
        CatalogTestFixture::tearDown();
    }

    boost::optional<Timestamp> readTimestampStore() {
        Lock::GlobalLock lk(_opCtx, MODE_IS);
        return _timestampStore.read(_opCtx);
    }

    OperationContext* _opCtx = nullptr;
    CollectionSizeCountStore _sizeCountStore;
    CollectionSizeCountTimestampStore _timestampStore;
    std::unique_ptr<SizeCountCheckpointCoordinator> _coordinator;
};

TEST_F(SizeCountCheckpointCoordinatorTest, MultipleRequestFlushCallsBeforeFlushAreCoalesced) {
    // _flushRequested is a bool: N calls collapse to a single pending flush.
    for (int i = 0; i < 10; ++i) {
        _coordinator->requestFlush();
    }
    ASSERT_TRUE(_coordinator->isFlushRequested_ForTest());
}

TEST_F(SizeCountCheckpointCoordinatorTest, FlushSyncWithEmptyTimestampStoreIsNoOp) {
    const auto initial = readTimestampStore();
    _coordinator->flushSync_ForTest(_opCtx);
    ASSERT_EQ(readTimestampStore(), initial);
}

class SizeCountCheckpointCoordinatorWithOplogTest : public SizeCountCheckpointCoordinatorTest {
protected:
    void setUp() override {
        SizeCountCheckpointCoordinatorTest::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), _collA.nss, CollectionOptions{.uuid = _collA.uuid}));
    }

    void writeInsert(Timestamp ts, int32_t sizeDelta = 10) {
        test_helpers::writeToOplog(
            operationContext(),
            test_helpers::makeOplogEntry(ts, _collA, repl::OpTypeEnum::kInsert, sizeDelta));
        repl::signalOplogWaiters();
    }

    const test_helpers::NsAndUUID _collA{
        .nss = NamespaceString::createNamespaceString_forTest("coordinator_test", "collA"),
        .uuid = UUID::gen(),
    };
};

TEST_F(SizeCountCheckpointCoordinatorWithOplogTest,
       FlushSyncWithNoNewDataPreservesPersistedTimestamp) {
    const Timestamp persistedTs(10, 5);
    {
        Lock::GlobalLock writeLock(_opCtx, MODE_IX);
        WriteUnitOfWork wuow(_opCtx);
        _timestampStore.write(_opCtx, persistedTs);
        wuow.commit();
    }

    // Write an oplog entry at persistedTs so bootstrap can seekExact to it (simulating that the
    // previous checkpoint run had processed up to this point and the entry is still in the oplog).
    writeInsert(persistedTs);

    auto coordinator = std::make_unique<SizeCountCheckpointCoordinator>(
        _sizeCountStore, _timestampStore, oplogUuid(), persistedTs);
    coordinator->flushSync_ForTest(_opCtx);

    ASSERT_EQ(readTimestampStore(), boost::optional<Timestamp>(persistedTs));
}

TEST_F(SizeCountCheckpointCoordinatorWithOplogTest, FlushSyncAdvancesTimestampAfterTailCycle) {
    const Timestamp ts(10, 1);
    writeInsert(ts);

    _coordinator->flushSync_ForTest(_opCtx);

    ASSERT_EQ(readTimestampStore(), boost::optional<Timestamp>(ts));
}

TEST_F(SizeCountCheckpointCoordinatorTest, FlushFailureIncrementsFlushFailureCountMetric) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "Skipping test due to OTel metrics being unavailable in this build";
    }

    // The failure metric is incremented by the flush thread's run() loop, which the synchronous
    // flushSync_ForTest path does not exercise, so drive the real flush thread.
    FailPointEnableBlock failFp("failDuringFlush");
    _coordinator->startup(getServiceContext());
    _coordinator->requestFlush();

    // Wait until the flush has reached the failpoint, so destruction cannot preempt the flush
    // before run()'s catch classifies the failure and increments the metric.
    failFp->waitForTimesEntered(failFp.initialTimesEntered() + 1);

    // The destructor joins the flush thread. The join cannot return until the thread has run
    // run()'s catch (incrementing the metric) and exited, so the assertion needs no polling.
    _coordinator.reset();

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 1);

    // A failed flush does not increment success metrics.
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushSuccessCount), 0);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushedDocsTotal), 0);
}

TEST_F(SizeCountCheckpointCoordinatorTest,
       DestructorJoinsBackgroundThreadsWithoutExplicitShutdown) {
    {
        auto localCoord = std::make_unique<SizeCountCheckpointCoordinator>(
            _sizeCountStore, _timestampStore, oplogUuid(), Timestamp::min());
        localCoord->startup(getServiceContext());
        // Destructor joins the worker threads.
    }
    // Reaching here without a hang or crash confirms the destructor joined successfully.
}

TEST_F(SizeCountCheckpointCoordinatorTest, DestructorDuringFlushCycleInterruptsAndCompletes) {
    OtelMetricsCapturer capturer;
    _coordinator->startup(getServiceContext());

    stdx::thread destroyer;
    {
        FailPointEnableBlock hangFp("hangAfterReplicatedFastCountSnapshot");

        _coordinator->requestFlush();
        hangFp->waitForTimesEntered(hangFp.initialTimesEntered() + 1);

        // Destroy the coordinator while the flush thread is stalled inside _runOneFlushCycle.
        // The destructor interrupts the flush thread's opCtx, but the thread cannot unblock
        // until the failpoint is disabled (pauseWhileSet does not check the opCtx).
        destroyer = stdx::thread([&] { _coordinator.reset(); });

        // Scope exit: disables failpoint. The flush thread then observes the interrupted opCtx
        // and surfaces InterruptedDueToReplStateChange, which run() treats as a benign
        // replication-state change (not a flush failure) before exiting the loop.
    }

    destroyer.join();

    // The destruction interrupt is a replication-state change, not a flush failure.
    if (capturer.canReadMetrics()) {
        ASSERT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 0);
    }
}

TEST_F(SizeCountCheckpointCoordinatorTest, ConcurrentRequestFlushAndDestructorNeverDeadlocks) {
    for (int i = 0; i < 50; ++i) {
        auto coordinator = std::make_shared<SizeCountCheckpointCoordinator>(
            _sizeCountStore, _timestampStore, oplogUuid(), Timestamp::min());
        coordinator->startup(getServiceContext());

        // The flusher thread holds a copy of the shared_ptr to keep the coordinator alive
        // while it calls requestFlush(). The main thread releases its copy concurrently,
        // which may trigger the destructor if the flusher has already finished.
        stdx::thread flusher([coordinator] {
            for (int j = 0; j < 5; ++j) {
                coordinator->requestFlush();
            }
        });
        stdx::thread destroyer([&] { coordinator.reset(); });

        flusher.join();
        destroyer.join();
    }
}

}  // namespace
}  // namespace mongo::replicated_fast_count
