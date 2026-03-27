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

#include "mongo/db/s/resharding/resharding_donor_post_cloning_delta_collector.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * Minimal stub for most external state methods - only getDocumentsDeltaFromDonors is exercised
 * by the delta collector.
 */
class StubExternalState : public ReshardingCoordinatorExternalState {
public:
    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext*,
        const ReshardingCoordinatorDocument&,
        std::vector<ReshardingZoneType>) override {
        MONGO_UNREACHABLE;
    }
    void tellAllDonorsToRefresh(OperationContext*,
                                const NamespaceString&,
                                const UUID&,
                                const std::vector<DonorShardEntry>&,
                                const std::shared_ptr<executor::TaskExecutor>&,
                                CancellationToken) override {
        MONGO_UNREACHABLE;
    }
    void tellAllRecipientsToRefresh(OperationContext*,
                                    const NamespaceString&,
                                    const UUID&,
                                    const std::vector<RecipientShardEntry>&,
                                    const std::shared_ptr<executor::TaskExecutor>&,
                                    CancellationToken) override {
        MONGO_UNREACHABLE;
    }
    void establishAllDonorsAsParticipants(OperationContext*,
                                          const NamespaceString&,
                                          const std::vector<DonorShardEntry>&,
                                          const std::shared_ptr<executor::TaskExecutor>&,
                                          CancellationToken) override {
        MONGO_UNREACHABLE;
    }
    void establishAllRecipientsAsParticipants(OperationContext*,
                                              const NamespaceString&,
                                              const std::vector<RecipientShardEntry>&,
                                              const std::shared_ptr<executor::TaskExecutor>&,
                                              CancellationToken) override {
        MONGO_UNREACHABLE;
    }
    std::map<ShardId, int64_t> getDocumentsToCopyFromDonors(
        OperationContext*,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const Timestamp&,
        const std::map<ShardId, ShardVersion>&) override {
        MONGO_UNREACHABLE;
    }
    void verifyClonedCollection(OperationContext*,
                                const std::shared_ptr<executor::TaskExecutor>&,
                                CancellationToken,
                                const ReshardingCoordinatorDocument&) override {
        MONGO_UNREACHABLE;
    }
    void verifyFinalCollection(OperationContext*, const ReshardingCoordinatorDocument&) override {
        MONGO_UNREACHABLE;
    }
};

/**
 * Returns a fixed delta map immediately, used for the success test cases.
 */
class SuccessMockExternalState : public StubExternalState {
public:
    explicit SuccessMockExternalState(std::map<ShardId, int64_t> delta)
        : _delta(std::move(delta)) {}

    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext*,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>&) override {
        return _delta;
    }

private:
    std::map<ShardId, int64_t> _delta;
};

/**
 * Fails with a retryable network error on the first call to getDocumentsDeltaFromDonors, then
 * returns a fixed delta on the second call. Used to verify the automatic-retry path.
 */
class FailOnceThenSucceedMockExternalState : public StubExternalState {
public:
    explicit FailOnceThenSucceedMockExternalState(std::map<ShardId, int64_t> delta)
        : _delta(std::move(delta)) {}

    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext*,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>&) override {
        if (_callCount++ == 0) {
            uasserted(ErrorCodes::HostUnreachable, "simulated retryable network error");
        }
        return _delta;
    }

    int callCount() const {
        return _callCount.load();
    }

private:
    std::map<ShardId, int64_t> _delta;
    std::atomic<int> _callCount{0};  // NOLINT
};

/**
 * Always fails with a non-retryable error. Used to verify that unrecoverable errors propagate
 * immediately without retrying.
 */
class UnrecoverableMockExternalState : public StubExternalState {
public:
    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext*,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>&) override {
        ++_callCount;
        uasserted(ErrorCodes::InternalError, "simulated unrecoverable error");
    }

    int callCount() const {
        return _callCount.load();
    }

private:
    std::atomic<int> _callCount{0};  // NOLINT
};

/**
 * Blocks in getDocumentsDeltaFromDonors until the opCtx is killed or the object is destroyed,
 * simulating the scenario where the operation is waiting (e.g. for the critical section) and
 * resharding is aborted.
 */
class BlockingMockExternalState : public StubExternalState {
public:
    ~BlockingMockExternalState() {
        {
            stdx::lock_guard lk(_mutex);
            _destroyed = true;
        }
        _cv.notify_all();

        // Wait for any in-progress getDocumentsDeltaFromDonors call to release the mutex and
        // exit, so we don't free _mutex/_cv while they are still in use.
        stdx::unique_lock lk(_mutex);
        _cv.wait(lk, [this] { return !_inCall; });
    }

    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>&) override {
        {
            stdx::lock_guard lk(_mutex);
            _entered = true;
            _inCall = true;
        }
        _cv.notify_all();

        // Clear _inCall on exit (normal return or exception) so the destructor can proceed.
        // Must be declared before `lk` so that on unwind, `lk` is destroyed first (releasing
        // the mutex), allowing this guard to re-acquire it.
        ScopeGuard inCallGuard([&] {
            stdx::lock_guard lk(_mutex);
            _inCall = false;
            _cv.notify_all();
        });

        stdx::unique_lock<stdx::mutex> lk(_mutex);
        opCtx->waitForConditionOrInterrupt(_cv, lk, [this] { return _destroyed; });

        return {};
    }

    void waitUntilEntered() {
        stdx::unique_lock lk(_mutex);
        _cv.wait(lk, [this] { return _entered; });
    }

private:
    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    bool _entered = false;
    bool _inCall = false;
    bool _destroyed = false;
};

class ReshardingDonorPostCloningDeltaCollectorTest
    : public service_context_test::WithSetupTransportLayer,
      public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        // A small thread pool used by CancelableOperationContext to asynchronously call
        // markKilled() on opCtxs when the cancellation token fires.
        ThreadPool::Options cancellationOpts;
        cancellationOpts.poolName = "DeltaCollectorTestCancellation";
        cancellationOpts.minThreads = 1;
        cancellationOpts.maxThreads = 1;
        _executorForCancellation = std::make_shared<ThreadPool>(cancellationOpts);
        _executorForCancellation->startup();

        // A task executor for running the async fetch chain.
        ThreadPool::Options threadPoolOpts;
        threadPoolOpts.poolName = "DeltaCollectorTest";
        threadPoolOpts.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName, getGlobalServiceContext()->getService());
        };

        _taskExecutor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(std::move(threadPoolOpts)),
            executor::makeNetworkInterface("DeltaCollectorTestNetwork"));
        _taskExecutor->startup();
        _scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(_taskExecutor);
    }

    void tearDown() override {
        (*_scopedExecutor)->shutdown();
        _scopedExecutor.reset();
        _taskExecutor->shutdown();
        _taskExecutor->join();
        _executorForCancellation->shutdown();
        _executorForCancellation->join();
        ServiceContextTest::tearDown();
    }

    /**
     * Builds a ReshardingCoordinatorDocument in the given state.
     * If documentsFinalAlreadySet is true, sets documentsFinal on the first donor, simulating a
     * resume after the fetch already completed.
     */
    ReshardingCoordinatorDocument makeDoc(CoordinatorStateEnum state,
                                          const std::vector<ShardId>& donorShardIds,
                                          bool documentsFinalAlreadySet = false) {
        std::vector<DonorShardEntry> donorShards;
        for (const auto& shardId : donorShardIds) {
            DonorShardEntry entry(shardId, {});
            if (documentsFinalAlreadySet) {
                entry.setDocumentsFinal(100);
            }
            donorShards.emplace_back(std::move(entry));
        }

        ReshardingCoordinatorDocument doc;
        doc.setCommonReshardingMetadata({UUID::gen(),
                                         NamespaceString::createNamespaceString_forTest("db.coll"),
                                         UUID::gen(),
                                         NamespaceString::createNamespaceString_forTest("db.tmp"),
                                         BSON("x" << 1)});
        doc.setState(state);
        doc.setDonorShards(std::move(donorShards));
        return doc;
    }

    std::shared_ptr<ReshardingDonorPostCloningDeltaCollector> makeCollector(
        ReshardingCoordinatorDocument doc,
        std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
        CancellationToken abortToken) {
        return std::make_shared<ReshardingDonorPostCloningDeltaCollector>(
            std::move(doc),
            std::move(externalState),
            abortToken,
            std::make_unique<HierarchicalCancelableOperationContextFactory>(
                abortToken, _executorForCancellation));
    }

    otel::traces::Span makeSpanForCollector() {
        auto telemetryCtx = otel::traces::Span::createTelemetryContext();
        return otel::traces::Span::start(telemetryCtx, "donorCountCollector");
    }

    const std::shared_ptr<executor::ScopedTaskExecutor>& scopedExecutor() const {
        return _scopedExecutor;
    }

private:
    std::shared_ptr<ThreadPool> _executorForCancellation;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _taskExecutor;
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;
};

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, SkipsFetchWhenNotInBlockingWritesState) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{ShardId("shard0"), 5}});
    auto doc = makeDoc(CoordinatorStateEnum::kCommitting, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto result = future.get();
    ASSERT_TRUE(result.empty());
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, SkipsFetchWhenDocumentsFinalAlreadySet) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{ShardId("shard0"), 5}});
    // Simulate a resume after the delta was already persisted.
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites,
                       {ShardId("shard0")},
                       true /* documentsFinalAlreadySet */);
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto result = future.get();
    ASSERT_TRUE(result.empty());
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, FetchesDeltaSuccessfully) {
    const ShardId shard0("shard0");
    const ShardId shard1("shard1");
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{shard0, 42}, {shard1, -7}});

    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {shard0, shard1});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto result = future.get();
    ASSERT_EQ(result.size(), 2U);
    ASSERT_EQ(result.at(shard0), 42);
    ASSERT_EQ(result.at(shard1), -7);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest,
       RetryableNetworkErrorTriggersRetryAndSucceeds) {
    const ShardId shard0("shard0");
    CancellationSource abortSource;
    auto externalState = std::make_shared<FailOnceThenSucceedMockExternalState>(
        std::map<ShardId, int64_t>{{shard0, 10}});

    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {shard0});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto result = future.get();
    ASSERT_EQ(externalState->callCount(), 2);
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result.at(shard0), 10);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, UnrecoverableErrorPropagatesWithoutRetry) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<UnrecoverableMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto status = future.getNoThrow();
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::InternalError);
    // A non-retryable error must not trigger any retry: exactly one call expected.
    ASSERT_EQ(externalState->callCount(), 1);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest,
       AbortTokenCancelsBlockedFetchAndCompletesWithError) {
    CancellationSource abortSource;
    auto blockingState = std::make_shared<BlockingMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, blockingState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    // Wait until getDocumentsDeltaFromDonors is actually blocking before cancelling.
    blockingState->waitUntilEntered();

    abortSource.cancel();

    auto status = future.getNoThrow();
    ASSERT_NOT_OK(status);
}

}  // namespace
}  // namespace mongo
