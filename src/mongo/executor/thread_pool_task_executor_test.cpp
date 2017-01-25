/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_test_common.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace executor {
namespace {

MONGO_INITIALIZER(ThreadPoolExecutorCommonTests)(InitializerContext*) {
    addTestsForExecutor("ThreadPoolExecutorCommon", [](std::unique_ptr<NetworkInterfaceMock> net) {
        return makeThreadPoolTestExecutor(std::move(net));
    });
    return Status::OK();
}

void setStatus(const TaskExecutor::CallbackArgs& cbData, Status* outStatus) {
    *outStatus = cbData.status;
}

TEST_F(ThreadPoolExecutorTest, TimelyCancelationOfScheduleWorkAt) {
    auto net = getNet();
    auto& executor = getExecutor();
    launchExecutorThread();
    auto status1 = getDetectableErrorStatus();
    const auto now = net->now();
    const auto cb1 = unittest::assertGet(executor.scheduleWorkAt(
        now + Milliseconds(5000), stdx::bind(setStatus, stdx::placeholders::_1, &status1)));

    const auto startTime = net->now();
    net->enterNetwork();
    net->runUntil(startTime + Milliseconds(200));
    net->exitNetwork();
    executor.cancel(cb1);
    executor.wait(cb1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status1);
    ASSERT_EQUALS(startTime + Milliseconds(200), net->now());
    executor.shutdown();
    joinExecutorThread();
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

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

    executor.shutdown();
    joinExecutorThread();
}

TEST_F(ThreadPoolExecutorTest, ShutdownAndScheduleRaceDoesNotCrash) {
    // This is a regression test for SERVER-23686. It works by scheduling a work item in the
    // ThreadPoolTaskExecutor that blocks waiting to be signaled by this thread. Once that work item
    // is scheduled, this thread enables a FailPoint that causes future calls of
    // ThreadPoolTaskExecutor::scheduleIntoPool_inlock to spin until its underlying thread pool
    // shuts down, forcing the race condition described in the aforementioned server ticket. The
    // failpoint ensures that this thread spins until the task executor thread begins spinning on
    // the underlying pool shutting down, then this thread tells the task executor to shut
    // down. Once the pool shuts down, the previously blocked scheduleIntoPool_inlock unblocks, and
    // discovers the pool to be shut down. The correct behavior is for all scheduled callbacks to
    // execute, and for this last callback at least to execute with CallbackCanceled as its status.
    // Before the fix for SERVER-23686, this test causes an fassert failure.

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
                      cb2 = cbData.executor->scheduleWork([&status2](
                          const TaskExecutor::CallbackArgs& cbData) { status2 = cbData.status; });
                  })
                  .getStatus());

    auto fpTPTE1 =
        getGlobalFailPointRegistry()->getFailPoint("scheduleIntoPoolSpinsUntilThreadPoolShutsDown");
    fpTPTE1->setMode(FailPoint::alwaysOn);
    barrier.countDownAndWait();
    MONGO_FAIL_POINT_PAUSE_WHILE_SET((*fpTPTE1));
    executor.shutdown();
    joinExecutorThread();
    ASSERT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status2);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
