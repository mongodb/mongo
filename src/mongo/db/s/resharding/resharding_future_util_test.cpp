// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_future_util.h"

#include "mongo/platform/atomic.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_impl.h"

#include <memory>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace {
class ReshardingFutureUtilTest : public unittest::Test {
protected:
    void setUp() override {
        _executor = std::make_shared<ThreadPool>([]() {
            ThreadPool::Options options;
            options.maxThreads = 2;
            return options;
        }());
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
    }

    std::shared_ptr<ThreadPool> getExecutor() const {
        return _executor;
    }

private:
    std::shared_ptr<ThreadPool> _executor;
};

TEST_F(ReshardingFutureUtilTest, CancelWhenAnyErrorThenQuiesceDuringExecutorShutdown) {
    CancellationSource cancelSource;
    auto token = cancelSource.token();
    PromiseAndFuture<void> taskThreadsReady;
    Atomic<int> tasksRunningCount{0};
    Atomic<bool> taskWasCancelled{false};
    auto checkSignalReady = [&]() {
        auto running = tasksRunningCount.addAndFetch(1);
        if (running == 2) {
            taskThreadsReady.promise.emplaceValue();
        }
    };
    PromiseAndFuture<void> executorShutDownTriggered;
    auto quiesced = ExecutorFuture(getExecutor()).then([&]() {
        return resharding::cancelWhenAnyErrorThenQuiesce(
            {ExecutorFuture(getExecutor())
                 .then([&]() {
                     checkSignalReady();
                     executorShutDownTriggered.future.wait();
                     uasserted(6791600, "Executor shut down");
                 })
                 .share(),
             ExecutorFuture(getExecutor())
                 .then([&]() {
                     checkSignalReady();
                     token.onCancel().wait();
                     taskWasCancelled.store(true);
                 })
                 .share()},
            getExecutor(),
            cancelSource);
    });
    taskThreadsReady.future.wait();
    getExecutor()->shutdown();
    executorShutDownTriggered.promise.emplaceValue();
    auto status = quiesced.getNoThrow();
    ASSERT_EQ(status.code(), 6791600);
    ASSERT_TRUE(taskWasCancelled.load());
}

TEST_F(ReshardingFutureUtilTest, IncludeReplicaSetWritesBlockedPredicateRetriesOnWriteBlock) {
    auto writeBlockError = Status(ErrorCodes::ReplicaSetWritesBlocked, "foo");
    // The dedicated predicate treats ReplicaSetWritesBlocked as retryable, while the predicate it
    // builds upon does not.
    ASSERT_TRUE(
        resharding::
            kRetryabilityPredicateIncludeReplicaSetWritesBlockedAndLockTimeoutAndWriteConcern(
                writeBlockError));
    ASSERT_FALSE(
        resharding::kRetryabilityPredicateIncludeLockTimeoutAndWriteConcern(writeBlockError));

    // It still retries on everything the base predicate considers retryable.
    auto lockTimeoutError = Status(ErrorCodes::LockTimeout, "bar");
    ASSERT_TRUE(
        resharding::
            kRetryabilityPredicateIncludeReplicaSetWritesBlockedAndLockTimeoutAndWriteConcern(
                lockTimeoutError));
}
}  // namespace
}  // namespace mongo
