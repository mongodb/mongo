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

#include <boost/thread/lock_types.hpp>
#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/repl/task_runner_test_fixture.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace {

    using namespace mongo;
    using namespace mongo::repl;

    using Task = TaskRunner::Task;

    TEST_F(TaskRunnerTest, InvalidConstruction) {
        // Null thread pool.
        ASSERT_THROWS(TaskRunner(nullptr, []() -> OperationContext* { return nullptr; }),
                      UserException);

        // Null function for creating operation contexts.
        ASSERT_THROWS(TaskRunner(&getThreadPool(), TaskRunner::CreateOperationContextFn()),
                      UserException);
    }

    TEST_F(TaskRunnerTest, GetDiagnosticString) {
        ASSERT_FALSE(getTaskRunner().getDiagnosticString().empty());
    }

    TEST_F(TaskRunnerTest, CallbackValues) {
        boost::mutex mutex;
        bool called = false;
        OperationContext* txn = nullptr;
        Status status = getDefaultStatus();
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::lock_guard<boost::mutex> lk(mutex);
            called = true;
            txn = theTxn;
            status = theStatus;
            return TaskRunner::NextAction::kCancel;
        };
        getTaskRunner().schedule(task);
        getThreadPool().join();
        ASSERT_FALSE(getTaskRunner().isActive());

        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_TRUE(called);
        ASSERT(txn);
        ASSERT_OK(status);
    }

    TEST_F(TaskRunnerTest, OperationContextFactoryReturnsNull) {
        resetTaskRunner(new TaskRunner(&getThreadPool(), []() -> OperationContext* {
            return nullptr;
        }));
        boost::mutex mutex;
        bool called = false;
        OperationContextNoop opCtxNoop;
        OperationContext* txn = &opCtxNoop;
        Status status = getDefaultStatus();
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::lock_guard<boost::mutex> lk(mutex);
            called = true;
            txn = theTxn;
            status = theStatus;
            return TaskRunner::NextAction::kCancel;
        };
        getTaskRunner().schedule(task);
        getThreadPool().join();
        ASSERT_FALSE(getTaskRunner().isActive());

        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_TRUE(called);
        ASSERT_FALSE(txn);
        ASSERT_OK(status);
    }

    std::vector<int> _testRunTaskTwice(TaskRunnerTest& test,
                                       TaskRunner::NextAction nextAction,
                                       stdx::function<void(const Task& task)> schedule) {
        boost::mutex mutex;
        int i = 0;
        OperationContext* txn[2] = {nullptr, nullptr};
        int txnId[2] = {-100, -100};
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::lock_guard<boost::mutex> lk(mutex);
            int j = i++;
            if (j >= 2) {
                return TaskRunner::NextAction::kInvalid;
            }
            txn[j] = theTxn;
            txnId[j] = TaskRunnerTest::getOperationContextId(txn[j]);
            TaskRunner::NextAction result = j == 0 ? nextAction : TaskRunner::NextAction::kCancel;
            return result;
        };
        schedule(task);
        ASSERT_TRUE(test.getTaskRunner().isActive());
        schedule(task);
        test.getThreadPool().join();
        ASSERT_FALSE(test.getTaskRunner().isActive());

        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_EQUALS(2, i);
        ASSERT(txn[0]);
        ASSERT(txn[1]);
        ASSERT_NOT_LESS_THAN(txnId[0], 0);
        ASSERT_NOT_LESS_THAN(txnId[1], 0);
        return {txnId[0], txnId[1]};
    }

    std::vector<int> _testRunTaskTwice(TaskRunnerTest& test, TaskRunner::NextAction nextAction) {
        auto schedule = [&](const Task& task) { test.getTaskRunner().schedule(task); };
        return _testRunTaskTwice(test, nextAction, schedule);
    }

    TEST_F(TaskRunnerTest, RunTaskTwiceDisposeOperationContext) {
        std::vector<int> txnId =
            _testRunTaskTwice(*this, TaskRunner::NextAction::kDisposeOperationContext);
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
        std::vector<int> txnId =
            _testRunTaskTwice(*this, TaskRunner::NextAction::kDisposeOperationContext, schedule);
        ASSERT_NOT_EQUALS(txnId[0], txnId[1]);
    }

    TEST_F(TaskRunnerTest, RunTaskTwiceKeepOperationContext) {
        std::vector<int> txnId =
            _testRunTaskTwice(*this, TaskRunner::NextAction::kKeepOperationContext);
        ASSERT_EQUALS(txnId[0], txnId[1]);
    }

    TEST_F(TaskRunnerTest, SkipSecondTask) {
        boost::mutex mutex;
        int i = 0;
        OperationContext* txn[2] = {nullptr, nullptr};
        Status status[2] = {getDefaultStatus(), getDefaultStatus()};
        boost::condition condition;
        bool schedulingDone = false;
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::unique_lock<boost::mutex> lk(mutex);
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
            boost::lock_guard<boost::mutex> lk(mutex);
            schedulingDone = true;
            condition.notify_all();
        }
        getThreadPool().join();
        ASSERT_FALSE(getTaskRunner().isActive());

        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_EQUALS(2, i);
        ASSERT(txn[0]);
        ASSERT_OK(status[0]);
        ASSERT_FALSE(txn[1]);
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status[1].code());
    }

    TEST_F(TaskRunnerTest, FirstTaskThrowsException) {
        boost::mutex mutex;
        int i = 0;
        OperationContext* txn[2] = {nullptr, nullptr};
        Status status[2] = {getDefaultStatus(), getDefaultStatus()};
        boost::condition condition;
        bool schedulingDone = false;
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::unique_lock<boost::mutex> lk(mutex);
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
            boost::lock_guard<boost::mutex> lk(mutex);
            schedulingDone = true;
            condition.notify_all();
        }
        getThreadPool().join();
        ASSERT_FALSE(getTaskRunner().isActive());

        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_EQUALS(2, i);
        ASSERT(txn[0]);
        ASSERT_OK(status[0]);
        ASSERT_FALSE(txn[1]);
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status[1].code());
    }

    TEST_F(TaskRunnerTest, Cancel) {
        boost::mutex mutex;
        boost::condition condition;
        Status status = getDefaultStatus();
        bool taskRunning = false;

        // Running this task causes the task runner to wait for another task that
        // is never scheduled.
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::lock_guard<boost::mutex> lk(mutex);
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
            boost::unique_lock<boost::mutex> lk(mutex);
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
        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_OK(status);
    }

    TEST_F(TaskRunnerTest, DestroyShouldWaitForTasksToComplete) {
        boost::mutex mutex;
        boost::condition condition;
        Status status = getDefaultStatus();
        bool taskRunning = false;

        // Running this task causes the task runner to wait for another task that
        // is never scheduled.
        auto task = [&](OperationContext* theTxn, const Status& theStatus) {
            boost::lock_guard<boost::mutex> lk(mutex);
            status = theStatus;
            taskRunning = true;
            condition.notify_all();
            return TaskRunner::NextAction::kKeepOperationContext;
        };

        getTaskRunner().schedule(task);
        ASSERT_TRUE(getTaskRunner().isActive());
        {
            boost::unique_lock<boost::mutex> lk(mutex);
            while (!taskRunning) {
                condition.wait(lk);
            }
        }

        destroyTaskRunner();

        getThreadPool().join();

        // This status will not be OK if canceling the task runner
        // before scheduling the task results in the task being canceled.
        boost::lock_guard<boost::mutex> lk(mutex);
        ASSERT_OK(status);
    }

} // namespace
