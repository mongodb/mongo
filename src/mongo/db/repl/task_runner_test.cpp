/**
 *    Copyright 2015 MongoDB Inc.
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

#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/repl/task_runner_test_fixture.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/barrier.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

using Task = TaskRunner::Task;

TEST_F(TaskRunnerTest, InvalidConstruction) {
    // Null thread pool.
    ASSERT_THROWS_CODE_AND_WHAT(
        TaskRunner(nullptr), UserException, ErrorCodes::BadValue, "null thread pool");
}

TEST_F(TaskRunnerTest, GetDiagnosticString) {
    ASSERT_FALSE(getTaskRunner().getDiagnosticString().empty());
}

TEST_F(TaskRunnerTest, CallbackValues) {
    stdx::mutex mutex;
    bool called = false;
    OperationContext* txn = nullptr;
    Status status = getDetectableErrorStatus();
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        called = true;
        txn = theTxn;
        status = theStatus;
        return TaskRunner::NextAction::kCancel;
    };
    getTaskRunner().schedule(task);
    getThreadPool().join();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_TRUE(called);
    ASSERT(txn);
    ASSERT_OK(status);
}

using OpIdVector = std::vector<unsigned int>;

OpIdVector _testRunTaskTwice(TaskRunnerTest& test,
                             TaskRunner::NextAction nextAction,
                             stdx::function<void(const Task& task)> schedule) {
    unittest::Barrier barrier(2U);
    stdx::mutex mutex;
    std::vector<OperationContext*> txns;
    OpIdVector txnIds;
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
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

    test.getThreadPool().join();
    ASSERT_FALSE(test.getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_EQUALS(2U, txns.size());
    ASSERT(txns[0]);
    ASSERT(txns[1]);
    return txnIds;
}

std::vector<unsigned int> _testRunTaskTwice(TaskRunnerTest& test,
                                            TaskRunner::NextAction nextAction) {
    auto schedule = [&](const Task& task) { test.getTaskRunner().schedule(task); };
    return _testRunTaskTwice(test, nextAction, schedule);
}

TEST_F(TaskRunnerTest, RunTaskTwiceDisposeOperationContext) {
    auto txnId = _testRunTaskTwice(*this, TaskRunner::NextAction::kDisposeOperationContext);
    ASSERT_NOT_EQUALS(txnId[0], txnId[1]);
}

// Joining thread pool before scheduling first task has no effect.
// Joining thread pool before scheduling second task ensures that task runner releases
// thread back to pool after disposing of operation context.
TEST_F(TaskRunnerTest, RunTaskTwiceDisposeOperationContextJoinThreadPoolBeforeScheduling) {
    auto schedule = [this](const Task& task) {
        getThreadPool().join();
        getTaskRunner().schedule(task);
    };
    auto txnId =
        _testRunTaskTwice(*this, TaskRunner::NextAction::kDisposeOperationContext, schedule);
    ASSERT_NOT_EQUALS(txnId[0], txnId[1]);
}

TEST_F(TaskRunnerTest, RunTaskTwiceKeepOperationContext) {
    auto txnId = _testRunTaskTwice(*this, TaskRunner::NextAction::kKeepOperationContext);
    ASSERT_EQUALS(txnId[0], txnId[1]);
}

TEST_F(TaskRunnerTest, SkipSecondTask) {
    stdx::mutex mutex;
    int i = 0;
    OperationContext* txn[2] = {nullptr, nullptr};
    Status status[2] = {getDetectableErrorStatus(), getDetectableErrorStatus()};
    stdx::condition_variable condition;
    bool schedulingDone = false;
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        int j = i++;
        if (j >= 2) {
            return TaskRunner::NextAction::kCancel;
        }
        txn[j] = theTxn;
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
        stdx::lock_guard<stdx::mutex> lk(mutex);
        schedulingDone = true;
        condition.notify_all();
    }
    getThreadPool().join();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_EQUALS(2, i);
    ASSERT(txn[0]);
    ASSERT_OK(status[0]);
    ASSERT_FALSE(txn[1]);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status[1].code());
}

TEST_F(TaskRunnerTest, FirstTaskThrowsException) {
    stdx::mutex mutex;
    int i = 0;
    OperationContext* txn[2] = {nullptr, nullptr};
    Status status[2] = {getDetectableErrorStatus(), getDetectableErrorStatus()};
    stdx::condition_variable condition;
    bool schedulingDone = false;
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        int j = i++;
        if (j >= 2) {
            return TaskRunner::NextAction::kCancel;
        }
        txn[j] = theTxn;
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
        invariant(false);
        return TaskRunner::NextAction::kKeepOperationContext;
    };
    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    getTaskRunner().schedule(task);
    {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        schedulingDone = true;
        condition.notify_all();
    }
    getThreadPool().join();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_EQUALS(2, i);
    ASSERT(txn[0]);
    ASSERT_OK(status[0]);
    ASSERT_FALSE(txn[1]);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status[1].code());
}

TEST_F(TaskRunnerTest, Cancel) {
    stdx::mutex mutex;
    stdx::condition_variable condition;
    Status status = getDetectableErrorStatus();
    bool taskRunning = false;

    // Running this task causes the task runner to wait for another task that
    // is never scheduled.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        status = theStatus;
        taskRunning = true;
        condition.notify_all();
        return TaskRunner::NextAction::kKeepOperationContext;
    };

    // Calling cancel() before schedule() has no effect.
    // The task should still be invoked with a successful status.
    getTaskRunner().cancel();

    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        while (!taskRunning) {
            condition.wait(lk);
        }
    }

    // It is fine to call cancel() multiple times.
    getTaskRunner().cancel();
    getTaskRunner().cancel();

    getThreadPool().join();
    ASSERT_FALSE(getTaskRunner().isActive());

    // This status will not be OK if canceling the task runner
    // before scheduling the task results in the task being canceled.
    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_OK(status);
}

TEST_F(TaskRunnerTest, JoinShouldWaitForTasksToComplete) {
    unittest::Barrier barrier(2U);
    stdx::mutex mutex;
    Status status1 = getDetectableErrorStatus();
    Status status2 = getDetectableErrorStatus();

    // "task1" should start running before we invoke join() the task runner.
    // Upon completion, "task1" requests the task runner to retain the operation context. This has
    // effect of keeping the task runner active.
    auto task1 = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        barrier.countDownAndWait();
        status1 = theStatus;
        return TaskRunner::NextAction::kKeepOperationContext;
    };

    // "task2" should start running after we invoke join() the task runner.
    // Upon completion, "task2" requests the task runner to dispose the operation context. After the
    // operation context is destroyed, the task runner will go into an inactive state.
    auto task2 = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
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
    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_OK(status1);
    ASSERT_OK(status2);
}

TEST_F(TaskRunnerTest, DestroyShouldWaitForTasksToComplete) {
    stdx::mutex mutex;
    stdx::condition_variable condition;
    Status status = getDetectableErrorStatus();
    bool taskRunning = false;

    // Running this task causes the task runner to wait for another task that
    // is never scheduled.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        status = theStatus;
        taskRunning = true;
        condition.notify_all();
        return TaskRunner::NextAction::kKeepOperationContext;
    };

    getTaskRunner().schedule(task);
    ASSERT_TRUE(getTaskRunner().isActive());
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        while (!taskRunning) {
            condition.wait(lk);
        }
    }

    destroyTaskRunner();

    getThreadPool().join();

    // This status will not be OK if canceling the task runner
    // before scheduling the task results in the task being canceled.
    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_OK(status);
}

}  // namespace
