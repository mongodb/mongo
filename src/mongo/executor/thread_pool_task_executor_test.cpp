/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/executor/thread_pool_task_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_test_common.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
namespace executor {
namespace {

MONGO_INITIALIZER(ThreadPoolExecutorCommonTests)(InitializerContext*) {
    addTestsForExecutor("ThreadPoolExecutorCommon", [](std::unique_ptr<NetworkInterfaceMock> net) {
        return makeThreadPoolTestExecutor(std::move(net));
    });
}

TEST_F(ThreadPoolExecutorTest, TimelyCancellationOfScheduleWorkAt) {
    auto net = getNet();
    auto& executor = getExecutor();
    launchExecutorThread();
    auto status1 = getDetectableErrorStatus();
    const auto now = net->now();
    const auto cb1 = unittest::assertGet(executor.scheduleWorkAt(
        now + Milliseconds(5000),
        [&](const TaskExecutor::CallbackArgs& cbData) { status1 = cbData.status; }));

    const auto startTime = net->now();
    net->enterNetwork();
    net->runUntil(startTime + Milliseconds(200));
    executor.cancel(cb1);
    net->runUntil(startTime + Milliseconds(300));
    net->exitNetwork();
    executor.wait(cb1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status1);
    ASSERT_EQUALS(startTime + Milliseconds(300), net->now());
}

TEST_F(ThreadPoolExecutorTest, Schedule) {
    auto& executor = getExecutor();
    launchExecutorThread();
    auto status1 = getDetectableErrorStatus();
    unittest::Barrier barrier{2};
    executor.schedule([&](Status status) {
        status1 = status;
        barrier.countDownAndWait();
    });
    barrier.countDownAndWait();
    ASSERT_OK(status1);
    // Wait for the executor to stop to ensure the scheduled job does not outlive current scope.
    shutdownExecutorThread();
    joinExecutorThread();
}

TEST_F(ThreadPoolExecutorTest, ScheduleAfterShutdown) {
    auto& executor = getExecutor();
    auto status1 = getDetectableErrorStatus();
    launchExecutorThread();
    shutdownExecutorThread();
    executor.schedule([&](Status status) { status1 = status; });
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status1);
}

TEST_F(ThreadPoolExecutorTest, OnEvent) {
    auto& executor = getExecutor();
    launchExecutorThread();
    auto status1 = getDetectableErrorStatus();
    auto event = executor.makeEvent().getValue();
    unittest::Barrier barrier{2};
    TaskExecutor::CallbackFn cb = [&](const TaskExecutor::CallbackArgs& args) {
        status1 = args.status;
        barrier.countDownAndWait();
    };
    ASSERT_OK(executor.onEvent(event, std::move(cb)).getStatus());
    // Callback was moved from.
    ASSERT(!cb);  // NOLINT(bugprone-use-after-move)
    executor.signalEvent(event);
    barrier.countDownAndWait();
    ASSERT_OK(status1);
    // Wait for the executor to stop to ensure the scheduled job does not outlive current scope.
    shutdownExecutorThread();
    joinExecutorThread();
}

TEST_F(ThreadPoolExecutorTest, OnEventCancel) {
    auto& executor = getExecutor();
    launchExecutorThread();

    auto swEvent = executor.makeEvent();
    ASSERT_OK(swEvent);

    auto pf = makePromiseFuture<void>();
    auto swCbh =
        executor.onEvent(swEvent.getValue(), [&](auto args) { pf.promise.setFrom(args.status); });
    ASSERT_OK(swCbh);

    ASSERT_FALSE(pf.future.isReady());
    executor.cancel(swCbh.getValue());
    ASSERT_EQ(pf.future.getNoThrow(), ErrorCodes::CallbackCanceled);
}

TEST_F(ThreadPoolExecutorTest, WaitForEvent) {
    auto& executor = getExecutor();
    launchExecutorThread();

    auto swEvent = executor.makeEvent();
    ASSERT_OK(swEvent);

    auto pf = makePromiseFuture<void>();
    auto th = stdx::thread([&] {
        executor.waitForEvent(swEvent.getValue());
        pf.promise.emplaceValue();
    });
    ON_BLOCK_EXIT([&] { th.join(); });

    ASSERT_FALSE(pf.future.isReady());
    executor.signalEvent(swEvent.getValue());

    ASSERT_OK(pf.future.getNoThrow());
}

TEST_F(ThreadPoolExecutorTest, OnEventAfterShutdown) {
    launchExecutorThread();
    auto& executor = getExecutor();
    auto status1 = getDetectableErrorStatus();
    auto event = executor.makeEvent().getValue();
    TaskExecutor::CallbackFn cb = [&](const TaskExecutor::CallbackArgs& args) {
        status1 = args.status;
    };
    shutdownExecutorThread();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress,
                  executor.onEvent(event, std::move(cb)).getStatus());

    // Callback was not moved from, it is still valid and we can call it to set status1.
    ASSERT(cb);  // NOLINT(bugprone-use-after-move)
    TaskExecutor::CallbackArgs args(&executor, {}, Status::OK());
    cb(args);
    ASSERT_OK(status1);
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    SharedCallbackState(const SharedCallbackState&) = delete;
    SharedCallbackState& operator=(const SharedCallbackState&) = delete;

public:
    SharedCallbackState() {}
    ~SharedCallbackState() {
        sharedCallbackStateDestroyed = true;
    }
};

TEST_F(ThreadPoolExecutorTest,
       ExecutorResetsCallbackFunctionInCallbackStateUponReturnFromCallbackFunction) {
    auto net = getNet();
    auto& executor = getExecutor();
    launchExecutorThread();

    auto sharedCallbackData = std::make_shared<SharedCallbackState>();
    auto callbackInvoked = false;

    const auto when = net->now() + Milliseconds(5000);
    const auto cb1 = unittest::assertGet(executor.scheduleWorkAt(
        when, [&callbackInvoked, sharedCallbackData](const executor::TaskExecutor::CallbackArgs&) {
            callbackInvoked = true;
        }));

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    net->enterNetwork();
    ASSERT_EQUALS(when, net->runUntil(when));
    net->exitNetwork();

    executor.wait(cb1);

    // Task executor should reset CallbackState::callback after running callback function.
    // This ensures that we release resources associated with 'CallbackState::callback' without
    // having to destroy every outstanding callback handle (which contains a shared pointer
    // to ThreadPoolTaskExecutor::CallbackState).
    ASSERT_TRUE(callbackInvoked);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

thread_local bool amRunningRecursively = false;

TEST_F(ThreadPoolExecutorTest, ShutdownAndScheduleWorkRaceDoesNotCrash) {
    // This test works by scheduling a work item in the ThreadPoolTaskExecutor that blocks waiting
    // to be signaled by this thread. Once that work item is scheduled, this thread enables a
    // FailPoint that causes future calls of ThreadPoolTaskExecutor::scheduleIntoPool_inlock to spin
    // until it is shutdown, forcing a race between the caller of schedule and the caller of
    // shutdown.  The failpoint ensures that this thread spins until the task executor thread begins
    // spinning on the state transitioning to shutting down, then this thread tells the task
    // executor to shut down. Once the executor shuts down, the previously blocked
    // scheduleIntoPool_inlock unblocks, and discovers the executor to be shut down. The correct
    // behavior is for all scheduled callbacks to execute, and for this last callback at least to
    // execute with CallbackCanceled as its status.

    unittest::Barrier barrier{2};
    auto status1 = getDetectableErrorStatus();
    StatusWith<TaskExecutor::CallbackHandle> cb2 = getDetectableErrorStatus();
    auto status2 = getDetectableErrorStatus();
    auto& executor = getExecutor();
    launchExecutorThread();

    ASSERT_OK(executor
                  .scheduleWork([&](const TaskExecutor::CallbackArgs& cbData) {
                      status1 = cbData.status;
                      if (!status1.isOK())
                          return;
                      barrier.countDownAndWait();

                      amRunningRecursively = true;
                      cb2 = cbData.executor->scheduleWork(
                          [&status2](const TaskExecutor::CallbackArgs& cbData) {
                              ASSERT_FALSE(amRunningRecursively);
                              status2 = cbData.status;
                          });
                      amRunningRecursively = false;
                  })
                  .getStatus());

    auto fpTPTE1 =
        globalFailPointRegistry().find("scheduleIntoPoolSpinsUntilThreadPoolTaskExecutorShutsDown");
    fpTPTE1->setMode(FailPoint::alwaysOn);
    barrier.countDownAndWait();
    (*fpTPTE1).pauseWhileSet();
    shutdownExecutorThread();
    joinExecutorThread();
    ASSERT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status2);
}

TEST_F(ThreadPoolExecutorTest, ShutdownAndScheduleRaceDoesNotCrash) {
    // Same as above, with schedule() instead of scheduleWork().
    unittest::Barrier barrier{2};
    auto status1 = getDetectableErrorStatus();
    auto status2 = getDetectableErrorStatus();
    auto& executor = getExecutor();
    launchExecutorThread();

    executor.schedule([&](Status status) {
        status1 = status;
        if (!status1.isOK())
            return;
        barrier.countDownAndWait();
        amRunningRecursively = true;
        executor.schedule([&status2](Status status) {
            ASSERT_FALSE(amRunningRecursively);
            status2 = status;
        });
        amRunningRecursively = false;
    });

    auto fpTPTE1 =
        globalFailPointRegistry().find("scheduleIntoPoolSpinsUntilThreadPoolTaskExecutorShutsDown");
    fpTPTE1->setMode(FailPoint::alwaysOn);
    barrier.countDownAndWait();
    (*fpTPTE1).pauseWhileSet();
    shutdownExecutorThread();
    joinExecutorThread();
    ASSERT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status2);
}

TEST_F(ThreadPoolExecutorTest, NotifyEventAfterShutdown) {
    auto& executor = getExecutor();
    auto swHandle = executor.makeEvent();
    ASSERT_OK(swHandle.getStatus());

    executor.shutdown();
    executor.join();

    executor.signalEvent(swHandle.getValue());
}

/**
 * This test reproduces a race where one thread shuts down the ThreadPoolTaskExecutor while another
 * thread is in the middle of canceling work scheduled on it. The race can cause a hang if the
 * shutdown thread attempts to drain work before the cancellation thread finishes processing the
 * cancellation. Calling NetworkInterfaceMock::drainUnfinishedNetworkOperations before joining the
 * executor will fix this.
 */
TEST_F(ThreadPoolExecutorTest, CancelFromAnotherThread) {
    auto& executor = getExecutor();
    launchExecutorThread();

    auto remote = HostAndPort("dummyHost:1234");
    auto rcr = RemoteCommandRequest(remote, DatabaseName::kAdmin, BSON("hello" << 1), nullptr);
    auto pf = makePromiseFuture<void>();
    auto swCbHandle = executor.scheduleRemoteCommand(
        rcr, [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        });
    ASSERT_OK(swCbHandle);

    auto tpte = checked_cast<ThreadPoolTaskExecutor*>(&executor);
    auto net = checked_cast<NetworkInterfaceMock*>(tpte->getNetworkInterface().get());

    Notification<void> startedCancellation;
    Notification<void> finishedShutdown;
    net->setOnCancelAction([&]() {
        // Signal to the main test thread that this thread began handling cancellation.
        startedCancellation.set();

        // Block cancellation until the main test thread finished shutting down the executor.
        finishedShutdown.get();
    });

    unittest::JoinThread th{[&] {
        executor.cancel(swCbHandle.getValue());
    }};

    startedCancellation.get();
    shutdownExecutorThread();
    finishedShutdown.set();
    joinExecutorThread();
}

}  // namespace
}  // namespace executor
}  // namespace mongo
