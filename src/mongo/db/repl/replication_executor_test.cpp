/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <map>

#include "mongo/base/init.h"
#include "mongo/executor/task_executor_test_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace repl {

namespace {

using executor::NetworkInterfaceMock;

const int64_t prngSeed = 1;

MONGO_INITIALIZER(ReplExecutorCommonTests)(InitializerContext*) {
    mongo::executor::addTestsForExecutor(
        "ReplicationExecutorCommon",
        [](std::unique_ptr<executor::NetworkInterfaceMock>* net) {
            return stdx::make_unique<ReplicationExecutor>(
                net->release(), new StorageInterfaceMock(), prngSeed);
        });
    return Status::OK();
}

TEST_F(ReplicationExecutorTest, ScheduleDBWorkAndExclusiveWorkConcurrently) {
    unittest::Barrier barrier(2U);
    NamespaceString nss("mydb", "mycoll");
    ReplicationExecutor& executor = getReplExecutor();
    Status status1 = getDetectableErrorStatus();
    OperationContext* txn = nullptr;
    using CallbackData = ReplicationExecutor::CallbackArgs;
    ASSERT_OK(executor.scheduleDBWork([&](const CallbackData& cbData) {
        status1 = cbData.status;
        txn = cbData.txn;
        barrier.countDownAndWait();
        if (cbData.status != ErrorCodes::CallbackCanceled)
            cbData.executor->shutdown();
    }).getStatus());
    ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
        barrier.countDownAndWait();
    }).getStatus());
    executor.run();
    ASSERT_OK(status1);
    ASSERT(txn);
}

TEST_F(ReplicationExecutorTest, ScheduleDBWorkWithCollectionLock) {
    NamespaceString nss("mydb", "mycoll");
    ReplicationExecutor& executor = getReplExecutor();
    Status status1 = getDetectableErrorStatus();
    OperationContext* txn = nullptr;
    bool collectionIsLocked = false;
    using CallbackData = ReplicationExecutor::CallbackArgs;
    ASSERT_OK(executor.scheduleDBWork([&](const CallbackData& cbData) {
        status1 = cbData.status;
        txn = cbData.txn;
        collectionIsLocked =
            txn ? txn->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X) : false;
        if (cbData.status != ErrorCodes::CallbackCanceled)
            cbData.executor->shutdown();
    }, nss, MODE_X).getStatus());
    executor.run();
    ASSERT_OK(status1);
    ASSERT(txn);
    ASSERT_TRUE(collectionIsLocked);
}

TEST_F(ReplicationExecutorTest, ScheduleExclusiveLockOperation) {
    ReplicationExecutor& executor = getReplExecutor();
    Status status1 = getDetectableErrorStatus();
    OperationContext* txn = nullptr;
    bool lockIsW = false;
    using CallbackData = ReplicationExecutor::CallbackArgs;
    ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
        status1 = cbData.status;
        txn = cbData.txn;
        lockIsW = txn ? txn->lockState()->isW() : false;
        if (cbData.status != ErrorCodes::CallbackCanceled)
            cbData.executor->shutdown();
    }).getStatus());
    executor.run();
    ASSERT_OK(status1);
    ASSERT(txn);
    ASSERT_TRUE(lockIsW);
}

TEST_F(ReplicationExecutorTest, ShutdownBeforeRunningSecondExclusiveLockOperation) {
    ReplicationExecutor& executor = getReplExecutor();
    using CallbackData = ReplicationExecutor::CallbackArgs;
    Status status1 = getDetectableErrorStatus();
    ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
        status1 = cbData.status;
        if (cbData.status != ErrorCodes::CallbackCanceled)
            cbData.executor->shutdown();
    }).getStatus());
    // Second db work item is invoked by the main executor thread because the work item is
    // moved from the exclusive lock queue to the ready work item queue when the first callback
    // cancels the executor.
    Status status2 = getDetectableErrorStatus();
    ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
        status2 = cbData.status;
        if (cbData.status != ErrorCodes::CallbackCanceled)
            cbData.executor->shutdown();
    }).getStatus());
    executor.run();
    ASSERT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status2.code());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
