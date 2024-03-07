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

// IWYU pragma: no_include "cxxabi.h"
#include <mutex>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/repl/task_runner_test_fixture.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo::repl {
namespace {

using Task = TaskRunner::Task;

TEST_F(TaskRunnerTest, InvalidConstruction) {
    // Null thread pool.
    ASSERT_THROWS_CODE_AND_WHAT(
        TaskRunner(nullptr), AssertionException, ErrorCodes::BadValue, "null thread pool");
}

TEST_F(TaskRunnerTest, GetDiagnosticString) {
    ASSERT_FALSE(getTaskRunner().getDiagnosticString().empty());
}

TEST_F(TaskRunnerTest, CallbackValues) {
    auto mutex = MONGO_MAKE_LATCH();
    bool called = false;
    OperationContext* opCtx = nullptr;
    Status status = getDetectableErrorStatus();
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<Latch> lk(mutex);
        called = true;
        opCtx = theTxn;
        status = theStatus;
        return TaskRunner::NextAction::kCancel;
    };
    getTaskRunner().schedule(task);
    getThreadPool().waitForIdle();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_TRUE(called);
    ASSERT(opCtx);
    ASSERT_OK(status);
}

using OpIdVector = std::vector<unsigned int>;

OpIdVector _testRunTaskTwice(TaskRunnerTest& test, unique_function<void(Task task)> schedule) {
    auto nextAction = TaskRunner::NextAction::kDisposeOperationContext;
    unittest::Barrier barrier(2U);
    auto mutex = MONGO_MAKE_LATCH();
    std::vector<OperationContext*> txns;
    OpIdVector txnIds;
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<Latch> lk(mutex);
        if (txns.size() >= 2U) {
            return TaskRunner::NextAction::kInvalid;
        }
        TaskRunner::NextAction result =
            txns.size() == 0 ? nextAction : TaskRunner::NextAction::kCancel;
        txns.push_back(theTxn);
        txnIds.push_back(theTxn->getOpID());
        barrier.countDownAndWait();
        return result;
    };

    schedule(task);
    ASSERT_TRUE(test.getTaskRunner().isActive());
    barrier.countDownAndWait();

    schedule(task);
    ASSERT_TRUE(test.getTaskRunner().isActive());
    barrier.countDownAndWait();

    test.getThreadPool().waitForIdle();
    ASSERT_FALSE(test.getTaskRunner().isActive());

    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_EQUALS(2U, txns.size());
    ASSERT(txns[0]);
    ASSERT(txns[1]);
    return txnIds;
}

TEST_F(TaskRunnerTest, RunTaskTwiceDisposeOperationContext) {
    auto schedule = [&](Task task) {
        getTaskRunner().schedule(std::move(task));
    };
    auto txnId = _testRunTaskTwice(*this, schedule);
    ASSERT_NOT_EQUALS(txnId[0], txnId[1]);
}

// Joining thread pool before scheduling first task has no effect.
// Joining thread pool before scheduling second task ensures that task runner releases
// thread back to pool after disposing of operation context.
TEST_F(TaskRunnerTest, RunTaskTwiceDisposeOperationContextJoinThreadPoolBeforeScheduling) {
    auto schedule = [this](Task task) {
        getThreadPool().waitForIdle();
        getTaskRunner().schedule(std::move(task));
    };
    auto txnId = _testRunTaskTwice(*this, schedule);
    ASSERT_NOT_EQUALS(txnId[0], txnId[1]);
}

TEST_F(TaskRunnerTest, SkipSecondTask) {
    auto mutex = MONGO_MAKE_LATCH();
    int i = 0;
    OperationContext* opCtx[2] = {nullptr, nullptr};
    Status status[2] = {getDetectableErrorStatus(), getDetectableErrorStatus()};
    stdx::condition_variable condition;
    bool schedulingDone = false;
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::unique_lock<Latch> lk(mutex);
        int j = i++;
        if (j >= 2) {
            return TaskRunner::NextAction::kCancel;
        }
        opCtx[j] = theTxn;
        status[j] = theStatus;

        // Wait for the test code to schedule the second task.
        while (!schedulingDone) {
            condition.wait(lk);
        }

        return TaskRunner::NextAction::kCancel;
    };
    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    getTaskRunner().schedule(task);
    {
        stdx::lock_guard<Latch> lk(mutex);
        schedulingDone = true;
        condition.notify_all();
    }
    getThreadPool().waitForIdle();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_EQUALS(2, i);
    ASSERT(opCtx[0]);
    ASSERT_OK(status[0]);
    ASSERT_FALSE(opCtx[1]);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status[1].code());
}

TEST_F(TaskRunnerTest, FirstTaskThrowsException) {
    auto mutex = MONGO_MAKE_LATCH();
    int i = 0;
    OperationContext* opCtx[2] = {nullptr, nullptr};
    Status status[2] = {getDetectableErrorStatus(), getDetectableErrorStatus()};
    stdx::condition_variable condition;
    bool schedulingDone = false;
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::unique_lock<Latch> lk(mutex);
        int j = i++;
        if (j >= 2) {
            return TaskRunner::NextAction::kCancel;
        }
        opCtx[j] = theTxn;
        status[j] = theStatus;

        // Wait for the test code to schedule the second task.
        while (!schedulingDone) {
            condition.wait(lk);
        }

        // Throwing an exception from the first task should cancel
        // unscheduled tasks and make the task runner inactive.
        // When the second (canceled) task throws an exception, it should be ignored.
        uassert(ErrorCodes::OperationFailed, "task failure", false);

        // not reached.
        MONGO_UNREACHABLE;
        return TaskRunner::NextAction::kDisposeOperationContext;
    };
    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    getTaskRunner().schedule(task);
    {
        stdx::lock_guard<Latch> lk(mutex);
        schedulingDone = true;
        condition.notify_all();
    }
    getThreadPool().waitForIdle();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_EQUALS(2, i);
    ASSERT(opCtx[0]);
    ASSERT_OK(status[0]);
    ASSERT_FALSE(opCtx[1]);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status[1].code());
}

TEST_F(TaskRunnerTest, Cancel) {
    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable condition;
    Status status = getDetectableErrorStatus();
    bool taskRunning = false;

    // Running this task causes the task runner to wait for another task that
    // is never scheduled.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<Latch> lk(mutex);
        status = theStatus;
        taskRunning = true;
        condition.notify_all();
        return TaskRunner::NextAction::kDisposeOperationContext;
    };

    // Calling cancel() before schedule() has no effect.
    // The task should still be invoked with a successful status.
    getTaskRunner().cancel();

    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    {
        stdx::unique_lock<Latch> lk(mutex);
        while (!taskRunning) {
            condition.wait(lk);
        }
    }

    // It is fine to call cancel() multiple times.
    getTaskRunner().cancel();
    getTaskRunner().cancel();

    getThreadPool().waitForIdle();
    ASSERT_FALSE(getTaskRunner().isActive());

    // This status will not be OK if canceling the task runner
    // before scheduling the task results in the task being canceled.
    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_OK(status);
}

TEST_F(TaskRunnerTest, JoinShouldWaitForTasksToComplete) {
    unittest::Barrier barrier(2U);
    auto mutex = MONGO_MAKE_LATCH();
    Status status1 = getDetectableErrorStatus();
    Status status2 = getDetectableErrorStatus();

    // "task1" should start running before we invoke join() the task runner.
    auto task1 = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<Latch> lk(mutex);
        barrier.countDownAndWait();
        status1 = theStatus;
        return TaskRunner::NextAction::kDisposeOperationContext;
    };

    // "task2" should start running after we invoke join() the task runner.
    auto task2 = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<Latch> lk(mutex);
        status2 = theStatus;
        return TaskRunner::NextAction::kDisposeOperationContext;
    };

    getTaskRunner().schedule(task1);
    getTaskRunner().schedule(task2);
    barrier.countDownAndWait();

    // join() waits for "task1" and "task2" to complete execution.
    getTaskRunner().join();

    // This status should be OK because we ensured that the task
    // was scheduled and invoked before we called cancel().
    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_OK(status1);
    ASSERT_OK(status2);
}

TEST_F(TaskRunnerTest, DestroyShouldWaitForTasksToComplete) {
    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable condition;
    Status status = getDetectableErrorStatus();
    bool taskRunning = false;

    // Running this task causes the task runner to wait for another task that
    // is never scheduled.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<Latch> lk(mutex);
        status = theStatus;
        taskRunning = true;
        condition.notify_all();
        return TaskRunner::NextAction::kDisposeOperationContext;
    };

    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    {
        stdx::unique_lock<Latch> lk(mutex);
        while (!taskRunning) {
            condition.wait(lk);
        }
    }

    destroyTaskRunner();

    getThreadPool().waitForIdle();

    // This status will not be OK if canceling the task runner
    // before scheduling the task results in the task being canceled.
    stdx::lock_guard<Latch> lk(mutex);
    ASSERT_OK(status);
}

}  // namespace
}  // namespace mongo::repl
