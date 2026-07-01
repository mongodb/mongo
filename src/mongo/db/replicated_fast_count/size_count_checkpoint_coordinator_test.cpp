/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#include <mutex>

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
        if (_coordinator) {
            _coordinator->shutdown();
        }
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

TEST_F(SizeCountCheckpointCoordinatorTest, StartupAndShutdownAreIdempotent) {
    _coordinator->startup(getServiceContext());
    _coordinator->startup(getServiceContext());

    ASSERT_TRUE(_coordinator->isRunning_ForTest());

    _coordinator->shutdown();
    _coordinator->shutdown();

    ASSERT_FALSE(_coordinator->isRunning_ForTest());
}

TEST_F(SizeCountCheckpointCoordinatorTest,
       ShutdownCanCompleteWhileStartupCallerIsStillPausedAfterPublication) {
    stdx::thread starter;
    stdx::thread stopper;

    {
        FailPointEnableBlock startupPause(
            "hangAfterSizeCountCheckpointCoordinatorStartupPublishesThreads");

        starter = stdx::thread([&] { _coordinator->startup(getServiceContext()); });

        startupPause->waitForTimesEntered(startupPause.initialTimesEntered() + 1);

        stopper = stdx::thread([&] { _coordinator->shutdown(); });

        // If shutdown incorrectly waited for the startup() caller to return, this join would
        // deadlock: starter is still blocked at the failpoint above.
        stopper.join();
    }  // failpoint releases; starter unblocks and startup() returns

    starter.join();
    ASSERT_FALSE(_coordinator->isRunning_ForTest());
}

TEST_F(SizeCountCheckpointCoordinatorTest, StartupIsNoOpWhileShutdownIsJoiningWorkers) {
    _coordinator->startup(getServiceContext());
    ASSERT_TRUE(_coordinator->isRunning_ForTest());

    stdx::thread stopper;

    {
        FailPointEnableBlock shutdownPause("hangBeforeSizeCountCheckpointCoordinatorShutdownJoins");

        stopper = stdx::thread([&] { _coordinator->shutdown(); });

        shutdownPause->waitForTimesEntered(shutdownPause.initialTimesEntered() + 1);

        // While shutdown is in `kStopping`, confirm startup doesn't create new threads and start
        // running again.
        _coordinator->startup(getServiceContext());
        ASSERT_FALSE(_coordinator->isRunning_ForTest());
    }

    stopper.join();

    ASSERT_FALSE(_coordinator->isRunning_ForTest());
}

TEST_F(SizeCountCheckpointCoordinatorTest, StartupAfterShutdownIsAlwaysRejected) {
    _coordinator->startup(getServiceContext());
    ASSERT_TRUE(_coordinator->isRunning_ForTest());

    _coordinator->shutdown();
    ASSERT_FALSE(_coordinator->isRunning_ForTest());

    // kShutdown is terminal: startup() must be a no-op even after a clean shutdown.
    _coordinator->startup(getServiceContext());
    ASSERT_FALSE(_coordinator->isRunning_ForTest());
}

TEST_F(SizeCountCheckpointCoordinatorTest, ShutdownOnNeverStartedCoordinatorSetsTerminalState) {
    // Verifies the kStopped -> kShutdown path in shutdown(). tearDown() exercises this
    // implicitly for every non-started test.
    ASSERT_FALSE(_coordinator->isRunning_ForTest());
    _coordinator->shutdown();
    ASSERT_FALSE(_coordinator->isRunning_ForTest());

    // Terminal: any subsequent startup() must be rejected.
    _coordinator->startup(getServiceContext());
    ASSERT_FALSE(_coordinator->isRunning_ForTest());
}

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

    // Wait until the flush has reached the failpoint, so shutdown() cannot preempt the flush
    // before run()'s catch classifies the failure and increments the metric.
    failFp->waitForTimesEntered(failFp.initialTimesEntered() + 1);

    // shutdown() joins the flush thread; the join cannot return until the thread has run run()'s
    // catch (incrementing the metric) and exited, so the assertion needs no polling.
    _coordinator->shutdown();

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
        // Destructor calls shutdown() and joins the worker threads.
    }
    // Reaching here without a hang or crash confirms the destructor joined successfully.
}

TEST_F(SizeCountCheckpointCoordinatorTest, ShutdownDuringFlushCycleInterruptsAndCompletes) {
    OtelMetricsCapturer capturer;
    _coordinator->startup(getServiceContext());

    stdx::thread stopper;
    {
        FailPointEnableBlock hangFp("hangAfterReplicatedFastCountSnapshot");

        _coordinator->requestFlush();
        hangFp->waitForTimesEntered(hangFp.initialTimesEntered() + 1);

        // Start shutdown while the flush thread is stalled inside _runOneFlushCycle.
        // shutdown() interrupts the flush thread's opCtx, but the thread cannot unblock
        // until the failpoint is disabled (pauseWhileSet does not check the opCtx).
        stopper = stdx::thread([&] { _coordinator->shutdown(); });

        // Scope exit: disables failpoint. The flush thread then observes the interrupted opCtx
        // and surfaces InterruptedDueToReplStateChange, which run() treats as a benign
        // replication-state change (not a flush failure) before exiting the loop.
    }

    stopper.join();
    ASSERT_FALSE(_coordinator->isRunning_ForTest());

    // The shutdown interrupt is a replication-state change, not a flush failure.
    if (capturer.canReadMetrics()) {
        ASSERT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 0);
    }
}

TEST_F(SizeCountCheckpointCoordinatorTest, RepeatedConcurrentStartupShutdownLeavesStoppedState) {
    for (int i = 0; i < 50; ++i) {
        stdx::thread starter([&] { _coordinator->startup(getServiceContext()); });

        stdx::thread stopper([&] { _coordinator->shutdown(); });

        starter.join();
        stopper.join();

        ASSERT_FALSE(_coordinator->isRunning_ForTest());
    }
}

TEST_F(SizeCountCheckpointCoordinatorTest, ConcurrentRequestFlushAndShutdownNeverDeadlocks) {
    for (int i = 0; i < 50; ++i) {
        auto coordinator = std::make_unique<SizeCountCheckpointCoordinator>(
            _sizeCountStore, _timestampStore, oplogUuid(), Timestamp::min());
        coordinator->startup(getServiceContext());

        stdx::thread flusher([&] {
            for (int j = 0; j < 5; ++j) {
                coordinator->requestFlush();
            }
        });
        stdx::thread stopper([&] { coordinator->shutdown(); });

        flusher.join();
        stopper.join();

        ASSERT_FALSE(coordinator->isRunning_ForTest());
    }
}

}  // namespace
}  // namespace mongo::replicated_fast_count
