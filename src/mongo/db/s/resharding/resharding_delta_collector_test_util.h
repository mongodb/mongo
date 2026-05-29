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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/scopeguard.h"

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace mongo {
namespace resharding_delta_collector_test_util {

/**
 * Minimal stub for the methods of ReshardingCoordinatorExternalState that the donor/recipient
 * post-cloning delta collectors don't exercise. Both delta-fetch methods (donor and recipient)
 * dispatch to a shared protected hook fetchDelta() so that subclasses can override one method
 * regardless of whether they're used by the donor or recipient collector tests.
 */
class StubExternalState : public ReshardingCoordinatorExternalState {
public:
    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext*,
        const ReshardingCoordinatorDocument&,
        std::vector<ReshardingZoneType>) override {
        MONGO_UNREACHABLE;
    }
    bool searchIndexExistsForCollection(OperationContext* opCtx,
                                        const NamespaceString& nss) override {
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
    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>&) override {
        return fetchDelta(opCtx);
    }
    std::map<ShardId, int64_t> getDocumentsDeltaFromRecipients(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>&) override {
        return fetchDelta(opCtx);
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
    void stopMigrations(OperationContext*,
                        const NamespaceString&,
                        const UUID&,
                        const OperationSessionInfo&) override {
        MONGO_UNREACHABLE;
    }
    void resumeMigrations(OperationContext*,
                          const NamespaceString&,
                          const UUID&,
                          const OperationSessionInfo&) override {
        MONGO_UNREACHABLE;
    }
    std::unique_ptr<CausalityBarrier> buildCausalityBarrier(
        std::vector<ShardId> participants,
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken token) override {
        MONGO_UNREACHABLE;
    }

protected:
    virtual std::map<ShardId, int64_t> fetchDelta(OperationContext*) {
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

protected:
    std::map<ShardId, int64_t> fetchDelta(OperationContext*) override {
        return _delta;
    }

private:
    std::map<ShardId, int64_t> _delta;
};

/**
 * Fails with a retryable network error on the first call, then returns a fixed delta on the
 * second call. Used to verify the automatic-retry path.
 */
class FailOnceThenSucceedMockExternalState : public StubExternalState {
public:
    explicit FailOnceThenSucceedMockExternalState(std::map<ShardId, int64_t> delta)
        : _delta(std::move(delta)) {}

    int callCount() const {
        return _callCount.load();
    }

protected:
    std::map<ShardId, int64_t> fetchDelta(OperationContext*) override {
        if (_callCount++ == 0) {
            uasserted(ErrorCodes::HostUnreachable, "simulated retryable network error");
        }
        return _delta;
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
    int callCount() const {
        return _callCount.load();
    }

protected:
    std::map<ShardId, int64_t> fetchDelta(OperationContext*) override {
        ++_callCount;
        uasserted(ErrorCodes::InternalError, "simulated unrecoverable error");
    }

private:
    std::atomic<int> _callCount{0};  // NOLINT
};

/**
 * Blocks in fetchDelta until the opCtx is killed or the object is destroyed, simulating the
 * scenario where the operation is waiting (e.g. for the critical section) and resharding is
 * aborted.
 */
class BlockingMockExternalState : public StubExternalState {
public:
    ~BlockingMockExternalState() override {
        {
            std::lock_guard lk(_mutex);
            _destroyed = true;
        }
        _cv.notify_all();

        // Wait for any in-progress fetchDelta call to release the mutex and exit, so we don't
        // free _mutex/_cv while they are still in use.
        std::unique_lock lk(_mutex);
        _cv.wait(lk, [this] { return !_inCall; });
    }

    void waitUntilEntered() {
        std::unique_lock lk(_mutex);
        _cv.wait(lk, [this] { return _entered; });
    }

protected:
    std::map<ShardId, int64_t> fetchDelta(OperationContext* opCtx) override {
        {
            std::lock_guard lk(_mutex);
            _entered = true;
            _inCall = true;
        }
        _cv.notify_all();

        // Clear _inCall on exit (normal return or exception) so the destructor can proceed.
        // Must be declared before `lk` so that on unwind, `lk` is destroyed first (releasing
        // the mutex), allowing this guard to re-acquire it.
        ScopeGuard inCallGuard([&] {
            std::lock_guard lk(_mutex);
            _inCall = false;
            _cv.notify_all();
        });

        std::unique_lock<std::mutex> lk(_mutex);
        opCtx->waitForConditionOrInterrupt(_cv, lk, [this] { return _destroyed; });

        return {};
    }

private:
    std::mutex _mutex;
    stdx::condition_variable _cv;
    bool _entered = false;
    bool _inCall = false;
    bool _destroyed = false;
};

class PostCloningDeltaCollectorTestBase : public service_context_test::WithSetupTransportLayer,
                                          public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        ThreadPool::Options cancellationOpts;
        cancellationOpts.poolName = "DeltaCollectorTestCancellation";
        cancellationOpts.minThreads = 1;
        cancellationOpts.maxThreads = 1;
        _executorForCancellation = std::make_shared<ThreadPool>(cancellationOpts);
        _executorForCancellation->startup();

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

    const std::shared_ptr<executor::ScopedTaskExecutor>& scopedExecutor() const {
        return _scopedExecutor;
    }

    const std::shared_ptr<ThreadPool>& executorForCancellation() const {
        return _executorForCancellation;
    }

    otel::traces::Span makeSpanForCollector() {
        auto telemetryCtx = otel::traces::Span::createTelemetryContext();
        return otel::traces::Span::start(telemetryCtx, "deltaCollector");
    }

private:
    std::shared_ptr<ThreadPool> _executorForCancellation;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _taskExecutor;
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;
};

}  // namespace resharding_delta_collector_test_util
}  // namespace mongo
