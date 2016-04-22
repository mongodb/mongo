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

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/database_task.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/repl/task_runner_test_fixture.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/old_thread_pool.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const std::string databaseName = "mydb";
const std::string collectionName = "mycoll";
const NamespaceString nss(databaseName, collectionName);

class DatabaseTaskTest : public TaskRunnerTest {};

TEST_F(DatabaseTaskTest, TaskRunnerErrorStatus) {
    // Should not attempt to acquire lock on error status from task runner.
    auto task = [](OperationContext* txn, const Status& status) {
        ASSERT_FALSE(txn);
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        return TaskRunner::NextAction::kInvalid;
    };
    auto testLockTask = [](DatabaseTask::Task task) {
        ASSERT_TRUE(TaskRunner::NextAction::kInvalid ==
                    task(nullptr, Status(ErrorCodes::BadValue, "")));
    };
    testLockTask(DatabaseTask::makeGlobalExclusiveLockTask(task));
    testLockTask(DatabaseTask::makeDatabaseLockTask(task, databaseName, MODE_X));
    testLockTask(DatabaseTask::makeCollectionLockTask(task, nss, MODE_X));
}

TEST_F(DatabaseTaskTest, RunGlobalExclusiveLockTask) {
    stdx::mutex mutex;
    bool called = false;
    OperationContext* txn = nullptr;
    bool lockIsW = false;
    Status status = getDetectableErrorStatus();
    // Task returning 'void' implies NextAction::NoAction.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        called = true;
        txn = theTxn;
        lockIsW = txn->lockState()->isW();
        status = theStatus;
        return TaskRunner::NextAction::kCancel;
    };
    getTaskRunner().schedule(DatabaseTask::makeGlobalExclusiveLockTask(task));
    getThreadPool().join();
    ASSERT_FALSE(getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_TRUE(called);
    ASSERT(txn);
    ASSERT_TRUE(lockIsW);
    ASSERT_OK(status);
}

void _testRunDatabaseLockTask(DatabaseTaskTest& test, LockMode mode) {
    stdx::mutex mutex;
    bool called = false;
    OperationContext* txn = nullptr;
    bool isDatabaseLockedForMode = false;
    Status status = test.getDetectableErrorStatus();
    // Task returning 'void' implies NextAction::NoAction.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        called = true;
        txn = theTxn;
        isDatabaseLockedForMode = txn->lockState()->isDbLockedForMode(databaseName, mode);
        status = theStatus;
        return TaskRunner::NextAction::kCancel;
    };
    test.getTaskRunner().schedule(DatabaseTask::makeDatabaseLockTask(task, databaseName, mode));
    test.getThreadPool().join();
    ASSERT_FALSE(test.getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_TRUE(called);
    ASSERT(txn);
    ASSERT_TRUE(isDatabaseLockedForMode);
    ASSERT_OK(status);
}

TEST_F(DatabaseTaskTest, RunDatabaseLockTaskModeX) {
    _testRunDatabaseLockTask(*this, MODE_X);
}

TEST_F(DatabaseTaskTest, RunDatabaseLockTaskModeS) {
    _testRunDatabaseLockTask(*this, MODE_S);
}

TEST_F(DatabaseTaskTest, RunDatabaseLockTaskModeIX) {
    _testRunDatabaseLockTask(*this, MODE_IX);
}

TEST_F(DatabaseTaskTest, RunDatabaseLockTaskModeIS) {
    _testRunDatabaseLockTask(*this, MODE_IS);
}

void _testRunCollectionLockTask(DatabaseTaskTest& test, LockMode mode) {
    stdx::mutex mutex;
    bool called = false;
    OperationContext* txn = nullptr;
    bool isCollectionLockedForMode = false;
    Status status = test.getDetectableErrorStatus();
    // Task returning 'void' implies NextAction::NoAction.
    auto task = [&](OperationContext* theTxn, const Status& theStatus) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        called = true;
        txn = theTxn;
        isCollectionLockedForMode =
            txn->lockState()->isCollectionLockedForMode(nss.toString(), mode);
        status = theStatus;
        return TaskRunner::NextAction::kCancel;
    };
    test.getTaskRunner().schedule(DatabaseTask::makeCollectionLockTask(task, nss, mode));
    test.getThreadPool().join();
    ASSERT_FALSE(test.getTaskRunner().isActive());

    stdx::lock_guard<stdx::mutex> lk(mutex);
    ASSERT_TRUE(called);
    ASSERT(txn);
    ASSERT_TRUE(isCollectionLockedForMode);
    ASSERT_OK(status);
}

TEST_F(DatabaseTaskTest, RunCollectionLockTaskModeX) {
    _testRunCollectionLockTask(*this, MODE_X);
}

TEST_F(DatabaseTaskTest, RunCollectionLockTaskModeS) {
    _testRunCollectionLockTask(*this, MODE_S);
}

TEST_F(DatabaseTaskTest, RunCollectionLockTaskModeIX) {
    _testRunCollectionLockTask(*this, MODE_IX);
}

TEST_F(DatabaseTaskTest, RunCollectionLockTaskModeIS) {
    _testRunCollectionLockTask(*this, MODE_IS);
}

}  // namespace
