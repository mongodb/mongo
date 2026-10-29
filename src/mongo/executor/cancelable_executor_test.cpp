// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/cancelable_executor.h"

#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"

#include <atomic>
#include <tuple>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace executor {
namespace {

class CancelableExecutorTest : public unittest::Test {
public:
    void setUp() override {
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _network = network.get();

        _executor = makeThreadPoolTestExecutor(std::move(network));
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> executor() {
        return _executor;
    }

    executor::NetworkInterfaceMock* network() {
        return _network;
    }

private:
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    executor::NetworkInterfaceMock* _network;
};

// Just check that a CancelableExecutor correctly schedules & runs work
// in the uncanceled case.
TEST(CancelableExecutor, SchedulesWorkCorrectly) {
    auto exec = InlineQueuedCountingExecutor::make();
    auto [promise, future] = makePromiseFuture<void>();
    CancellationSource source;
    auto fut2 =
        std::move(future).thenRunOn(CancelableExecutor::make(exec, source.token())).then([] {
            return 42;
        });
    promise.emplaceValue();
    ASSERT_EQ(fut2.get(), 42);
    ASSERT_EQ(exec->tasksRun.load(), 1);
}

// Check that if the token passed to a CancelableExecutor is canceled before
// any work on the executor is ready to run, no work runs.
TEST(CancelableExecutor, WorkCanceledBeforeScheduleDoesNotRun) {
    auto exec = InlineQueuedCountingExecutor::make();
    auto [promise, future] = makePromiseFuture<void>();
    CancellationSource source;
    auto fut2 =
        std::move(future).thenRunOn(CancelableExecutor::make(exec, source.token())).then([] {
            return 42;
        });
    source.cancel();
    promise.emplaceValue();
    ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::CallbackCanceled);
    ASSERT_EQ(exec->tasksRun.load(), 1);
}

// Check that if the token passed CancelableExecutor is cancelled in between
// continuations scheduled on the executor, those ready before the token is cancelled
// are run and those ready after the token is canceled are not.
TEST_F(CancelableExecutorTest, WorkCanceledAfterPreviousWorkOnExecutorHasRunDoesNotRun) {
    mongo::unittest::threadAssertionMonitoredTest([this](auto& assertionMonitor) {
        auto [promise, future] = makePromiseFuture<void>();
        auto [innerPromise, innerFuture] = makePromiseFuture<void>();
        CancellationSource source;
        unittest::Barrier barrier(2);
        auto fut2 = std::move(future)
                        .thenRunOn(CancelableExecutor::make(executor(), source.token()))
                        .then([&barrier, innerFuture = std::move(innerFuture)] {
                            barrier.countDownAndWait();
                            innerFuture.wait();
                        })
                        .then([&assertionMonitor] {
                            assertionMonitor.exec([] { FAIL("This shouldn't run."); });
                        });
        promise.emplaceValue();
        // Ensure we've executed the first continuation and are waiting on innerFuture.
        barrier.countDownAndWait();
        // Now that we're waiting on innerFuture, we'll cancel the executor...
        source.cancel();
        // And ensure that the final continuation doesn't run.
        innerPromise.emplaceValue();
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::CallbackCanceled);
    });
}

// Ensure that if a callback should be invoked with an error status due both to
// the CancelableExecutor being canceled before the callback is ready, and
// because the backing executor refuses work for some other reason, the backing
// executor's error status takes precedence.
TEST(CancelableExecutor, ExecutorRejectionsTakePrecedenceOverCancellation) {
    auto exec = RejectingExecutor::make();
    auto [promise, future] = makePromiseFuture<void>();
    CancellationSource source;
    auto fut2 =
        std::move(future).thenRunOn(CancelableExecutor::make(exec, source.token())).then([] {
            return 42;
        });
    source.cancel();
    promise.emplaceValue();
    ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::ShutdownInProgress);
}

// Check that no continuations (even error-handling ones) scheduled on a CancelableExecutor
// run after it is canceled, and that the cancellation error status is passed to the next
// error-handling callback on the future chain that runs in a non-canceled execution context/
// on an executor that accepts the work.
TEST(CancelableExecutor, ErrorsArePropagatedToAcceptingExecutor) {
    auto exec = InlineQueuedCountingExecutor::make();
    auto [promise, future] = makePromiseFuture<void>();
    CancellationSource source;
    auto cancelExec = CancelableExecutor::make(exec, source.token());
    source.cancel();
    // It's safe to FAIL in the continuations here, because this executor runs
    // scheduled work inline on the main thread.
    auto fut2 = std::move(future)
                    .thenRunOn(exec)
                    .then([] { return 42; })
                    .thenRunOn(cancelExec)
                    .then([](int) { FAIL("This shouldn't run"); })
                    .onError([](Status) { FAIL("This shouldn't run"); })
                    .thenRunOn(exec)
                    .then([]() { FAIL("This shouldn't run."); })
                    .onError([](Status s) { return s; });
    promise.emplaceValue();
    ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::CallbackCanceled);
}

// Just ensure that an uncanceled CancelableExecutor correctly propagates callback
// results forward in the future chain.
TEST(CancelableExecutor, UncanceledExecutorCanBeChainedCorrectly) {
    auto exec = InlineQueuedCountingExecutor::make();
    auto [promise, future] = makePromiseFuture<void>();
    CancellationSource source;
    auto cancelExec = CancelableExecutor::make(exec, source.token());
    auto fut2 = std::move(future)
                    .thenRunOn(exec)
                    .then([] { return 42; })
                    .thenRunOn(cancelExec)
                    .then([](int i) { return i + 2; });
    promise.emplaceValue();
    ASSERT_EQ(fut2.get(), 44);
}
}  // namespace
}  // namespace executor
}  // namespace mongo
