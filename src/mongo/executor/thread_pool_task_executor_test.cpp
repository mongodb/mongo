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
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {
namespace {

MONGO_INITIALIZER(ThreadPoolExecutorCommonTests)(InitializerContext*) {
    addTestsForExecutor("ThreadPoolExecutorCommon",
                        [](std::unique_ptr<NetworkInterfaceMock>* net) {
                            return makeThreadPoolTestExecutor(std::move(*net));
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

}  // namespace
}  // namespace executor
}  // namespace mongo
